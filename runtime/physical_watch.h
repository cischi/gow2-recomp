// Watching of guest physical pages for GPU invalidation
// (Xenia src/xenia/memory.cc @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause: PhysicalHeap::EnableAccessCallbacks/TriggerCallbacks and
// Memory::AccessViolationCallback, rewritten for gow2's arena).
//
// The GPU (Xenia code, through the xe::Memory shim) asks to watch physical
// ranges: Watch makes their 4 KB pages read-only in the three views
// 0xA0000000/0xC0000000/0xE0000000 (protection is per view).
// A write to a watched page, from guest or host code (NtReadFile's memcpy),
// raises a write access violation: the vectored handler of runtime/platform
// calls OnWriteFault, which calls the registered callbacks (Trigger), reopens
// the pages in all three views and has the instruction re-executed.
// Reopened pages are no longer watched until the GPU watches them again.
// Physical free and allocation (GuestMemory) call Trigger explicitly.
//
// Watched state: one bit per physical page, shared by the three views (Xenia
// keeps one bit per view and invalidates once per view; here a write from
// any mirror invalidates and reopens the page in all three).
//
// ---- Lock -------------------------------------------------------------------
// A single recursive lock, GlobalLock() (= xe::global_critical_region::mutex(),
// the shim forwards here). It is taken by Watch, Trigger, OnWriteFault (hence the
// exception handler), Register/Unregister and Xenia's GPU code.
// Callbacks run with GlobalLock held.
//
// Lock order: GlobalLock is the innermost of the runtime's locks.
//   runtime lock (kernel heap, object tables, dispatcher, ...)
//     -> GlobalLock -> internal locks of Xenia's GPU code -> log lock
// A thread can fault on a watched page while holding a runtime lock
// (for example VirtualAllocator::mutex_ while GuestMemory zeroes a physical
// allocation): it takes GlobalLock afterwards, as the order requires. So whoever
// holds GlobalLock (callbacks, Xenia's GPU code) must NEVER take a runtime
// lock or call the kernel (allocators, objects, guest waits)
// (only the log and the GPU's own locks). Writing to a watched page while
// holding GlobalLock is allowed (the lock is recursive).
// For the same reason kernel::stats::WriteSummary, apu::StopXmaDecoder and
// apu::Shutdown (they wait for workers that take GlobalLock) must not be called
// with GlobalLock held.
#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

struct GuestMemory;

namespace physwatch {

std::recursive_mutex& GlobalLock();

// Xenia's signature (Memory::PhysicalMemoryInvalidationCallback): receives
// the invalidated physical range (whole pages) and returns the range
// (start, length) the caller agrees to stop watching.
using Callback = std::pair<uint32_t, uint32_t> (*)(void* ctx, uint32_t phys, uint32_t len, bool exact);

// Attaches the arena (whose physical views are watched), clears the watched
// state and installs the write handler (plat::InstallWriteFaultHandler).
// Must be called before plat::InstallCrashHandler. A new Init replaces the
// previous arena.
void Init(GuestMemory&);
// Detaches `mem` if it is the attached arena (called by ~GuestMemory); the
// watched pages become unwatched (without callbacks).
void Detach(const GuestMemory& mem);
// true if `mem` is the arena attached by Init.
bool IsAttached(const GuestMemory& mem);

// Handle for Unregister. Register and Unregister are serialized with Trigger
// (GlobalLock): after Unregister the callback is not running and will not be
// called again.
void* Register(Callback, void* ctx);
void Unregister(void* handle);

// Makes the 4 KB pages of [phys, phys+length) (physical, & 0x1FFFFFFF not
// applied: truncated beyond 512 MiB) read-only in the 3 views and marks them
// watched. No effect without an attached arena.
void Watch(uint32_t phys, uint32_t length);
// Calls the callbacks for the pages of [phys, phys+length) if at least one is
// watched, then reopens (read/write, in the 3 views) and stops watching the
// intersection of the ranges returned by the callbacks, always including
// the requested pages and with a maximum excess up to the 4 MiB block that
// contains them; with unwatch_exact only the requested pages.
// true if at least one page was watched.
bool Trigger(uint32_t phys, uint32_t length, bool unwatch_exact);
// Handler for write access violations (runs in the vectored handler, does not
// allocate). true = handled, the instruction is re-executed: the page was
// watched (Trigger) or another thread has just reopened it. false if the
// address is outside the physical views of the attached arena or the page
// is neither watched nor writable (a real crash).
bool OnWriteFault(uintptr_t host_address);

// For tests: true if the physical page containing `phys` is watched.
bool IsWatched(uint32_t phys);

}  // namespace physwatch
