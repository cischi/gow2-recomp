// Guest memory (runtime/kernel/memory.cpp): virtual, physical and process
// heap and budget, on a real GuestMemory arena (4 GB reserved, no game
// bytes). The exports are called like the generated code, with a
// PPCContext and the arena base.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"

// only for the fault test's vectored handler (the runtime uses the OS only from platform/)
#include <windows.h>

PPC_EXTERN_FUNC(__imp__NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtFreeVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtQueryVirtualMemory);
PPC_EXTERN_FUNC(__imp__NtProtectVirtualMemory);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);
PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__MmQueryAddressProtect);
PPC_EXTERN_FUNC(__imp__MmQueryAllocationSize);
PPC_EXTERN_FUNC(__imp__MmQueryStatistics);
PPC_EXTERN_FUNC(__imp__RtlAllocateHeap);
PPC_EXTERN_FUNC(__imp__RtlFreeHeap);
PPC_EXTERN_FUNC(__imp__RtlSizeHeap);
PPC_EXTERN_FUNC(__imp__RtlReAllocateHeap);
PPC_EXTERN_FUNC(__imp__RtlUnicodeStringToAnsiString);
PPC_EXTERN_FUNC(__imp__RtlFreeAnsiString);

namespace {

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kMiB = 1u << 20;

using namespace kernel;

// Guest arena + kernel allocators; `scratch` is a committed page
// (GuestMemory's bump allocator) for the exports' in/out parameters.
struct Env {
  GuestMemory mem;
  uint32_t scratch = 0;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    scratch = mem.Alloc(0x1000, 0x1000);
    REQUIRE(scratch != 0);
  }
  ~Env() { ShutdownMemory(); }

  // Guest accesses: translation like the generated code (0xE view offset).
  uint32_t Get32(uint32_t a) const { return *GuestPtr<be<uint32_t>>(a); }
  void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }
  void Put16(uint32_t a, uint16_t v) { *GuestPtr<be<uint16_t>>(a) = v; }
  uint16_t Get16(uint32_t a) const { return *GuestPtr<be<uint16_t>>(a); }
  void Put8(uint32_t a, uint8_t v) { *GuestPtr<uint8_t>(a) = v; }
  uint8_t Get8(uint32_t a) const { return *GuestPtr<uint8_t>(a); }

  uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
    PPCContext ctx{};
    uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                         &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, mem.base);
    return ctx.r3.u32;
  }

  // NtAllocateVirtualMemory(&base, &size, type, protect, 0): status, base and size written.
  struct VirtualResult {
    uint32_t status, base, size;
  };
  VirtualResult AllocVirtual(uint32_t hint, uint32_t size, uint32_t type, uint32_t protect = kPageReadWrite) {
    Put32(scratch, hint);
    Put32(scratch + 4, size);
    const uint32_t st = Call(__imp__NtAllocateVirtualMemory, {scratch, scratch + 4, type, protect, 0});
    return {st, Get32(scratch), Get32(scratch + 4)};
  }
  VirtualResult FreeVirtual(uint32_t base, uint32_t size, uint32_t type) {
    Put32(scratch, base);
    Put32(scratch + 4, size);
    const uint32_t st = Call(__imp__NtFreeVirtualMemory, {scratch, scratch + 4, type, 0});
    return {st, Get32(scratch), Get32(scratch + 4)};
  }
  // X_MEMORY_BASIC_INFORMATION in scratch+0x100: 7 fields of 4 bytes.
  uint32_t QueryVirtual(uint32_t addr) { return Call(__imp__NtQueryVirtualMemory, {addr, scratch + 0x100}); }
  uint32_t Info(int field) const { return Get32(scratch + 0x100 + 4 * field); }
};

// Catch2 registers a vectored handler that treats every access violation as a
// fatal error: this one, with higher priority, notes the fault in the
// watched region, commits the page and makes the instruction retry (like the
// guard test in runtime_test.cpp, but on reserved-only pages).
uintptr_t g_watch_begin = 0, g_watch_end = 0;
volatile uintptr_t g_fault = 0;

LONG CALLBACK CommitOnFault(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t addr = uintptr_t(rec->ExceptionInformation[1]);
  if (addr < g_watch_begin || addr >= g_watch_end) return EXCEPTION_CONTINUE_SEARCH;
  g_fault = addr;
  void* page = reinterpret_cast<void*>(addr & ~uintptr_t(0xFFF));
  VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
  // in the physical mirrors the page is already committed but protected: reopen it
  DWORD old = 0;
  VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
  return EXCEPTION_CONTINUE_EXECUTION;
}

uintptr_t FaultingWrite(volatile uint8_t* p) {
  g_fault = 0;
  *p = 1;
  return g_fault;
}

}  // namespace

TEST_CASE("memory: GuestMemory only reserves, Commit and Alloc commit") {
  GuestMemory m;
  REQUIRE(m.Init());
  g_watch_begin = uintptr_t(m.base);
  g_watch_end = uintptr_t(m.base + GuestMemory::kSize);
  void* handler = AddVectoredExceptionHandler(1, &CommitOnFault);
  REQUIRE(handler != nullptr);

  // reserved-only page: the write faults
  CHECK(FaultingWrite(m.base + 0x90000000) == uintptr_t(m.base + 0x90000000));
  // after Commit (rounded to 4 KB pages) it does not, and the memory is zero
  REQUIRE(m.Commit(0x91000010, 0x1000));
  CHECK(m.base[0x91000000] == 0);
  CHECK(m.base[0x91001FFF] == 0);
  CHECK(FaultingWrite(m.base + 0x91000000) == 0);
  CHECK(FaultingWrite(m.base + 0x91001FFF) == 0);
  CHECK(FaultingWrite(m.base + 0x91002000) == uintptr_t(m.base + 0x91002000));
  CHECK_FALSE(m.Commit(0xFFFFF000, 0x2000));  // beyond 4 GB
  // Alloc commits what it hands out
  m.SetHeapStart(0x92000000);
  const uint32_t a = m.Alloc(0x1800, 0x1000);
  REQUIRE(a == 0x92000000);
  CHECK(FaultingWrite(m.base + a) == 0);
  CHECK(FaultingWrite(m.base + a + 0x17FF) == 0);
  // Decommit: faults again
  REQUIRE(m.Decommit(0x91000000, 0x1000));
  CHECK(FaultingWrite(m.base + 0x91000000) == uintptr_t(m.base + 0x91000000));

  RemoveVectoredExceptionHandler(handler);
}

TEST_CASE("memory: NtAllocateVirtualMemory without hint: 4 KB pages below 0x40000000, LARGE_PAGES above") {
  Env env;
  // without X_MEM_LARGE_PAGES: 4 KB page heap (Xenia v00000000), as
  // UE3 expects (address < 0x40000000 = VirtualAlloc block)
  auto r = env.AllocVirtual(0, 0x1234, kMemReserve | kMemCommit);
  REQUIRE(r.status == kStatusSuccess);
  CHECK(r.base >= kVirtual4kHeapBegin);
  CHECK(r.base < kVirtual4kHeapEnd);
  CHECK(r.base % 0x1000 == 0);
  CHECK(r.size == 0x2000);
  CHECK(env.mem.base[r.base] == 0);
  env.mem.base[r.base + 0x1FFF] = 7;  // committed: writable
  REQUIRE(env.QueryVirtual(r.base) == kStatusSuccess);
  CHECK(env.Info(1) == r.base);
  CHECK(env.Info(4) == kMemCommit);
  CHECK(env.Call(__imp__MmQueryAllocationSize, {r.base}) == 0x2000);
  // two 4 KB allocations do not waste 64 KB
  auto small = env.AllocVirtual(0, 0x1000, kMemReserve | kMemCommit);
  REQUIRE(small.status == kStatusSuccess);
  CHECK(small.base < kVirtual4kHeapEnd);
  CHECK(small.base != r.base);

  auto r2 = env.AllocVirtual(0, 0x1234, kMemReserve | kMemCommit | kMemLargePages);
  REQUIRE(r2.status == kStatusSuccess);
  CHECK(r2.base >= kVirtualHeapBegin);
  CHECK(r2.base < kVirtualHeapEnd);
  CHECK(r2.base % 0x10000 == 0);
  CHECK(r2.size == 0x10000);

  // free of a 4 KB block: from the heap that contains it
  auto f = env.FreeVirtual(small.base, 0, kMemRelease);
  CHECK(f.status == kStatusSuccess);
  CHECK(f.size == 0x1000);
  CHECK(env.Call(__imp__MmQueryAllocationSize, {small.base}) == 0);

  // top-down: from the top, below the process heap's arena
  auto r3 = env.AllocVirtual(0, 0x10000, kMemReserve | kMemCommit | kMemTopDown | kMemLargePages);
  REQUIRE(r3.status == kStatusSuccess);
  CHECK(r3.base > r2.base);
  CHECK(r3.base + 0x10000 <= kVirtualHeapEnd);

  // invalid parameters
  CHECK(env.AllocVirtual(0, 0, kMemCommit).status == kStatusInvalidParameter);
  CHECK(env.AllocVirtual(0, 0x1000, 0).status == kStatusInvalidParameter);
  CHECK(env.AllocVirtual(0x90000000, 0x1000, kMemCommit).status == kStatusInvalidParameter);
}

TEST_CASE("memory: NtAllocateVirtualMemory with hint, reserve and then commit") {
  Env env;
  // free pages: reserves from the hint rounded to 64 KB covering [hint, hint+size)
  auto r = env.AllocVirtual(0x50012345, 0x100, kMemReserve | kMemCommit);
  REQUIRE(r.status == kStatusSuccess);
  CHECK(r.base == 0x50010000);
  CHECK(r.base + r.size >= 0x50012345 + 0x100);
  // occupied: a second reservation at the same address fails
  CHECK(env.AllocVirtual(0x50010000, 0x1000, kMemReserve).status == kStatusNoMemory);

  // reservation without commit, then commit of a page inside the reservation
  auto res = env.AllocVirtual(0x60000000, 0x40000, kMemReserve);
  REQUIRE(res.status == kStatusSuccess);
  CHECK(res.base == 0x60000000);
  CHECK(res.size == 0x40000);
  REQUIRE(env.QueryVirtual(0x60011000) == kStatusSuccess);
  CHECK(env.Info(4) == kMemReserve);

  // commit of 4 KB inside a reservation of the 64 KB heap: whole 64 KB
  // page, like Xenia (heap page_size)
  auto c = env.AllocVirtual(0x60011000, 0x1000, kMemCommit);
  REQUIRE(c.status == kStatusSuccess);
  CHECK(c.base == 0x60010000);
  CHECK(c.size == 0x10000);

  REQUIRE(env.QueryVirtual(0x60011800) == kStatusSuccess);
  CHECK(env.Info(0) == 0x60011000);      // base_address: the queried page
  CHECK(env.Info(1) == 0x60000000);      // allocation_base
  CHECK(env.Info(2) == kPageReadWrite);  // allocation_protect
  CHECK(env.Info(3) == 0xF000);          // region_size: up to the end of the commit
  CHECK(env.Info(4) == kMemCommit);
  CHECK(env.Info(5) == kPageReadWrite);
  CHECK(env.Info(6) == kMemPrivate);
  REQUIRE(env.QueryVirtual(0x60020000) == kStatusSuccess);
  CHECK(env.Info(3) == 0x40000 - 0x20000);
  CHECK(env.Info(4) == kMemReserve);
  CHECK(env.Call(__imp__MmQueryAddressProtect, {0x6001F000}) == kPageReadWrite);
  CHECK(env.Call(__imp__MmQueryAddressProtect, {0x60020000}) == 0);
  CHECK(env.Call(__imp__MmQueryAllocationSize, {0x60012000}) == 0x40000);

  // the UE3 case: 2 MB reservation, commit of 0x3B140 bytes -> 0x40000
  auto big = env.AllocVirtual(0, 0x200000, kMemReserve | kMemLargePages);
  REQUIRE(big.status == kStatusSuccess);
  auto bc = env.AllocVirtual(big.base, 0x3B140, kMemCommit | kMemLargePages);
  REQUIRE(bc.status == kStatusSuccess);
  CHECK(bc.base == big.base);
  CHECK(bc.size == 0x40000);
  env.mem.base[big.base + 0x3C002] = 1;  // in the committed 64 KB page

  // NtProtectVirtualMemory records the protection and returns the old one
  env.Put32(env.scratch + 0x200, 0x60011000);
  env.Put32(env.scratch + 0x204, 0x1000);
  CHECK(env.Call(__imp__NtProtectVirtualMemory,
                 {env.scratch + 0x200, env.scratch + 0x204, kPageReadOnly, env.scratch + 0x208, 0}) ==
        kStatusSuccess);
  CHECK(env.Get32(env.scratch + 0x208) == kPageReadWrite);
  CHECK(env.Call(__imp__MmQueryAddressProtect, {0x60011000}) == kPageReadOnly);
}

TEST_CASE("memory: writing to a reserved-only page faults, after the commit it does not") {
  Env env;
  g_watch_begin = uintptr_t(env.mem.base + kVirtual4kHeapBegin);
  g_watch_end = uintptr_t(env.mem.base + kVirtualHeapEnd);
  void* handler = AddVectoredExceptionHandler(1, &CommitOnFault);
  REQUIRE(handler != nullptr);

  auto res = env.AllocVirtual(0, 0x20000, kMemReserve);
  REQUIRE(res.status == kStatusSuccess);
  CHECK(FaultingWrite(env.mem.base + res.base + 0x10000) == uintptr_t(env.mem.base + res.base + 0x10000));

  auto c = env.AllocVirtual(res.base, 0x10000, kMemCommit);
  REQUIRE(c.status == kStatusSuccess);
  CHECK(FaultingWrite(env.mem.base + res.base) == 0);
  CHECK(FaultingWrite(env.mem.base + res.base + 0xFFFF) == 0);

  // decommit: the page becomes inaccessible again
  REQUIRE(env.FreeVirtual(res.base, 0x10000, kMemDecommit).status == kStatusSuccess);
  CHECK(FaultingWrite(env.mem.base + res.base) == uintptr_t(env.mem.base + res.base));

  RemoveVectoredExceptionHandler(handler);
}

TEST_CASE("memory: free and reuse, contents zero again") {
  Env env;
  auto a = env.AllocVirtual(0, 0x30000, kMemReserve | kMemCommit);
  REQUIRE(a.status == kStatusSuccess);
  std::memset(env.mem.base + a.base, 0xAB, 0x30000);
  const uint64_t used = BytesInUse();

  auto f = env.FreeVirtual(a.base, 0, kMemRelease);
  REQUIRE(f.status == kStatusSuccess);
  CHECK(f.base == a.base);
  CHECK(f.size == 0x30000);
  CHECK(BytesInUse() == used - 0x30000);
  REQUIRE(env.QueryVirtual(a.base) == kStatusSuccess);
  CHECK(env.Info(4) == kMemFree);
  // release of an address that is not a base: error
  CHECK(env.FreeVirtual(a.base + 0x1000, 0, kMemRelease).status != kStatusSuccess);
  CHECK(env.FreeVirtual(0, 0, kMemRelease).status != kStatusSuccess);

  auto b = env.AllocVirtual(0, 0x30000, kMemReserve | kMemCommit);
  REQUIRE(b.status == kStatusSuccess);
  CHECK(b.base == a.base);  // first free space from the bottom
  for (uint32_t i = 0; i < 0x30000; i += 0x1000) {
    if (env.mem.base[b.base + i] != 0) FAIL("non-zero byte at +0x" << std::hex << i);
  }
}

TEST_CASE("memory: beyond the 512 MiB budget -> STATUS_NO_MEMORY / 0, consistent statistics") {
  Env env;
  REQUIRE(BytesInUse() == 0);

  auto big = env.AllocVirtual(0, 400 * kMiB, kMemReserve | kMemCommit);
  REQUIRE(big.status == kStatusSuccess);
  CHECK(BytesInUse() == 400 * kMiB);

  // commit beyond the budget: fails without effects
  auto over = env.AllocVirtual(0, 200 * kMiB, kMemReserve | kMemCommit);
  CHECK(over.status == kStatusNoMemory);
  CHECK(BytesInUse() == 400 * kMiB);
  // the reservation alone does not consume budget; the commit inside it does
  auto res = env.AllocVirtual(0, 200 * kMiB, kMemReserve);
  REQUIRE(res.status == kStatusSuccess);
  CHECK(env.AllocVirtual(res.base, 200 * kMiB, kMemCommit).status == kStatusNoMemory);
  REQUIRE(env.QueryVirtual(res.base) == kStatusSuccess);
  CHECK(env.Info(4) == kMemReserve);

  // physical beyond the budget -> 0; within the budget it works
  CHECK(env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 150 * kMiB, kPageReadWrite, 0, 0xFFFFFFFF, 0}) == 0);
  const uint32_t phys = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, kMiB, kPageReadWrite, 0, 0xFFFFFFFF, 0});
  REQUIRE(phys != 0);
  CHECK(BytesInUse() == 401 * kMiB);

  // process heap beyond the budget -> 0
  CHECK(env.Call(__imp__RtlAllocateHeap, {0, 0, 120 * kMiB}) == 0);

  // MmQueryStatistics: 4 KB pages, available = total - in use
  const uint32_t st = env.scratch + 0x400;
  env.Put32(st, 104);
  REQUIRE(env.Call(__imp__MmQueryStatistics, {st}) == kStatusSuccess);
  CHECK(env.Get32(st) == 104);
  CHECK(env.Get32(st + 4) == 0x20000);                              // total_physical_pages
  CHECK(env.Get32(st + 12) == 0x20000 - (401 * kMiB) / 0x1000);     // title.available_pages
  env.Put32(st, 50);
  CHECK(env.Call(__imp__MmQueryStatistics, {st}) != kStatusSuccess);  // wrong size

  // freeing makes it available again
  env.Call(__imp__MmFreePhysicalMemory, {0, phys});
  REQUIRE(env.FreeVirtual(big.base, 0, kMemRelease).status == kStatusSuccess);
  CHECK(BytesInUse() == 0);
  env.Put32(st, 104);
  REQUIRE(env.Call(__imp__MmQueryStatistics, {st}) == kStatusSuccess);
  CHECK(env.Get32(st + 12) == 0x20000);
  auto again = env.AllocVirtual(0, 200 * kMiB, kMemReserve | kMemCommit);
  CHECK(again.status == kStatusSuccess);
}

TEST_CASE("memory: physical allocation, physical address, size and free") {
  Env env;
  const uint32_t a = env.Call(__imp__MmAllocatePhysicalMemoryEx,
                              {0, 0x12345, kPageReadWrite | kMemLargePages, 0, 0xFFFFFFFF, 0x10000});
  REQUIRE(a != 0);
  CHECK(a >= kPhysicalHeapBegin);
  CHECK(uint64_t(a) + 0x20000 <= kPhysicalHeapEnd);
  CHECK(a % 0x10000 == 0);
  CHECK(env.Call(__imp__MmGetPhysicalAddress, {a}) == (a & 0x1FFFFFFF));
  CHECK(env.Call(__imp__MmGetPhysicalAddress, {0xA1234567}) == 0x01234567);
  CHECK(env.Call(__imp__MmQueryAllocationSize, {a}) == 0x20000);  // rounded to 64 KB
  CHECK(env.Call(__imp__MmQueryAddressProtect, {a + 0x100}) == kPageReadWrite);
  CHECK(env.mem.base[a] == 0);
  env.mem.base[a + 0x1FFFF] = 1;

  // default alignment 4 KB; MmAllocatePhysicalMemory = Ex with full range
  const uint32_t b = env.Call(__imp__MmAllocatePhysicalMemory, {0, 0x10, kPageReadWrite});
  REQUIRE(b != 0);
  CHECK(b % 0x1000 == 0);
  CHECK(b != a);
  // invalid protection -> 0 (Xenia)
  CHECK(env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x1000, 0, 0, 0xFFFFFFFF, 0}) == 0);

  const uint64_t used = BytesInUse();
  env.Call(__imp__MmFreePhysicalMemory, {0, a});
  CHECK(BytesInUse() == used - 0x20000);
  CHECK(env.Call(__imp__MmQueryAllocationSize, {a}) == 0);
  // reuse: zero again
  const uint32_t c = env.Call(__imp__MmAllocatePhysicalMemoryEx,
                              {0, 0x20000, kPageReadWrite | kMemLargePages, 0, 0xFFFFFFFF, 0x10000});
  REQUIRE(c != 0);
  CHECK(env.mem.base[c + 0x1FFFF] == 0);
}

TEST_CASE("memory: the physical address view follows the page size") {
  // Xenia memory.cc: 4 KB pages in 0xE0000000 (heap of 0x1FD00000 bytes,
  // physical = address - 0xE0000000 + 0x1000), 64 KB in 0xA0000000, 16 MB
  // in 0xC0000000. UE3 (appFree) picks how to free a block from the high bits
  // of the address: a 4 KB block outside 0xE... ends up in its pools.
  Env env;
  const uint32_t small = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x2000, kPageReadWrite, 0, 0xFFFFFFFF, 0});
  REQUIRE(small != 0);
  CHECK((small & 0xE0000000) == 0xE0000000);
  const uint32_t large = env.Call(__imp__MmAllocatePhysicalMemoryEx,
                                  {0, 0x10000, kPageReadWrite | kMemLargePages, 0, 0xFFFFFFFF, 0});
  REQUIRE(large != 0);
  CHECK((large & 0xE0000000) == 0xA0000000);
  const uint32_t huge = env.Call(__imp__MmAllocatePhysicalMemoryEx,
                                 {0, 0x1000000, kPageReadWrite | kMem16MbPages, 0, 0xFFFFFFFF, 0});
  REQUIRE(huge != 0);
  CHECK((huge & 0xE0000000) == 0xC0000000);

  // physical of the 0xE view: 4 KB offset; in the other two the physical is the address & 0x1FFFFFFF
  const uint32_t phys = env.Call(__imp__MmGetPhysicalAddress, {small});
  CHECK(phys == (small & 0x1FFFFFFF) + 0x1000);
  CHECK(phys >= 0x1000);
  CHECK(phys + 0x2000 <= 0x1FD01000);
  CHECK(env.Call(__imp__MmGetPhysicalAddress, {large}) == (large & 0x1FFFFFFF));
  CHECK(env.Call(__imp__MmGetPhysicalAddress, {huge}) == (huge & 0x1FFFFFFF));

  // same block from every view: size and protection
  const uint32_t small_a = 0xA0000000u | phys, small_c = 0xC0000000u | phys;
  for (const uint32_t v : {small, small_a, small_c}) {
    INFO("address " << std::hex << v);
    CHECK(env.Call(__imp__MmQueryAllocationSize, {v}) == 0x2000);
    CHECK(env.Call(__imp__MmQueryAddressProtect, {v + 0x1234}) == kPageReadWrite);
  }
  const uint32_t huge_phys = huge & 0x1FFFFFFF;
  REQUIRE(huge_phys >= 0x1000);  // below 0x1000 the physical has no address in the 0xE view
  for (const uint32_t v : {huge, 0xA0000000u | huge_phys, (0xE0000000u | huge_phys) - 0x1000}) {
    INFO("address " << std::hex << v);
    CHECK(env.Call(__imp__MmQueryAllocationSize, {v}) == 0x1000000);
    CHECK(env.Call(__imp__MmQueryAddressProtect, {v}) == kPageReadWrite);
  }

  // same memory: written from 0xE, read from the 0xA address of its physical
  env.Put8(small + 0x123, 0x5A);
  env.Put32(small + 0x1FFC, 0xCAFEF00D);
  CHECK(env.Get8(small_a + 0x123) == 0x5A);
  CHECK(env.Get32(small_a + 0x1FFC) == 0xCAFEF00D);
  CHECK(env.Get32(small_c + 0x1FFC) == 0xCAFEF00D);
  CHECK(env.mem.base[small_a + 0x123] == 0x5A);

  // NtQueryVirtualMemory: addresses in the queried view
  REQUIRE(env.QueryVirtual(small + 0x1234) == kStatusSuccess);
  CHECK(env.Info(0) == small + 0x1000);  // base_address
  CHECK(env.Info(1) == small);           // allocation_base
  CHECK(env.Info(3) == 0x1000);          // region_size
  REQUIRE(env.QueryVirtual(small_a + 0x1234) == kStatusSuccess);
  CHECK(env.Info(0) == small_a + 0x1000);
  CHECK(env.Info(1) == small_a);
  CHECK(env.Info(3) == 0x1000);
  REQUIRE(env.QueryVirtual(small_c) == kStatusSuccess);
  CHECK(env.Info(0) == small_c);
  CHECK(env.Info(1) == small_c);
  CHECK(env.Info(3) == 0x2000);

  // no physical below 0x1000 is reachable from 0xE
  CHECK(env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x1000, kPageReadWrite, 0, 0xFFF, 0}) == 0);

  // free from a view different from the returned one
  const uint64_t used = BytesInUse();
  env.Call(__imp__MmFreePhysicalMemory, {0, small_c});
  env.Call(__imp__MmFreePhysicalMemory, {0, (0xE0000000u | huge_phys) - 0x1000});
  env.Call(__imp__MmFreePhysicalMemory, {0, 0xC0000000u | (large & 0x1FFFFFFF)});
  CHECK(BytesInUse() == used - 0x2000 - 0x1000000 - 0x10000);
  CHECK(env.Call(__imp__MmQueryAllocationSize, {small}) == 0);
}

TEST_CASE("memory: physical mirrors 0xA/0xC/0xE: same memory, always committed, commit to zero") {
  // Xenia memory.cc (map_info, MapViews): 0xA0000000, 0xC0000000 and 0xE0000000
  // are views of the same 512 MiB physical memory, section fully
  // committed (SEC_COMMIT): the GPU must never fault when reading.
  GuestMemory m;
  REQUIRE(m.Init());
  g_watch_begin = uintptr_t(m.base);
  g_watch_end = uintptr_t(m.base + GuestMemory::kSize);
  void* handler = AddVectoredExceptionHandler(1, &CommitOnFault);
  REQUIRE(handler != nullptr);

  // physical 0x1FD40000: never allocated, but committed and accessible from all three views
  CHECK(m.base[0xBFD40000] == 0);
  CHECK(FaultingWrite(m.base + 0xBFD40000) == 0);
  CHECK(FaultingWrite(m.base + 0xDFD41000) == 0);
  CHECK(FaultingWrite(m.base + 0xFFD42000) == 0);
  CHECK(m.base[0xDFD40000] == 1);

  // commit (= handing out an allocation) from the 0xA view: zero, visible from 0xC and 0xE
  REQUIRE(m.Commit(0xBFD40000, 0x10000));
  CHECK(m.base[0xBFD40000] == 0);
  CHECK(m.base[0xFFD42000] == 0);
  CHECK(m.base[0xDFD4FFFF] == 0);
  m.base[0xBFD40684] = 0x5A;
  *reinterpret_cast<volatile uint32_t*>(m.base + 0xBFD40700) = 0x12345678;
  CHECK(m.base[0xDFD40684] == 0x5A);
  CHECK(m.base[0xFFD40684] == 0x5A);
  CHECK(*reinterpret_cast<volatile uint32_t*>(m.base + 0xDFD40700) == 0x12345678);
  // write from 0xE, read from 0xA
  m.base[0xFFD4FFFF] = 0x77;
  CHECK(m.base[0xBFD4FFFF] == 0x77);
  // commit from a different view (0xC), on a range that crosses a boundary
  REQUIRE(m.Commit(0xDFFFF000, 0x2000));  // physical 0x1FFFF000 and 0x00000000
  m.base[0xBFFFF000] = 1;
  m.base[0xA0000FFF] = 2;
  CHECK(m.base[0xFFFFF000] == 1);
  CHECK(m.base[0xE0000FFF] == 2);
  CHECK(m.base[0xC0000FFF] == 2);

  // decommit (= free): the pages stay committed and readable/writable from all views
  REQUIRE(m.Decommit(0xDFD40000, 0x10000));
  CHECK(FaultingWrite(m.base + 0xBFD40684) == 0);
  CHECK(FaultingWrite(m.base + 0xDFD41000) == 0);
  CHECK(FaultingWrite(m.base + 0xFFD42000) == 0);
  CHECK(m.base[0xFFD40684] == 1);
  // new commit: all zero again
  REQUIRE(m.Commit(0xBFD40000, 0x10000));
  CHECK(m.base[0xBFD40700] == 0);
  CHECK(m.base[0xDFD4FFFF] == 0);
  CHECK(m.base[0xFFD40684] == 0);
  CHECK(m.base[0xFFD42000] == 0);

  RemoveVectoredExceptionHandler(handler);
}

TEST_CASE("memory: MmAllocatePhysicalMemoryEx physical block used via 0xC and 0xE, free and budget") {
  Env env;
  const uint64_t before = BytesInUse();
  // GoW2's D3D worker case: 3 MB LARGE_PAGES|NOCACHE|RW aligned to 0x80
  const uint32_t a = env.Call(__imp__MmAllocatePhysicalMemoryEx,
                              {0, 0x300000, 0x20000404, 0, 0xFFFFFFFF, 0x80});
  REQUIRE(a >= kPhysicalHeapBegin);
  CHECK(BytesInUse() == before + 0x300000);
  const uint32_t phys = env.Call(__imp__MmGetPhysicalAddress, {a});
  CHECK(phys == (a & 0x1FFFFFFF));
  REQUIRE(phys >= 0x1000);
  // 0xE view: physical p is at guest address 0xE0000000 + p - 0x1000
  const uint32_t c = 0xC0000000u | phys, e = (0xE0000000u | phys) - 0x1000;
  env.Put32(c + 0x50684, 0xCAFEF00D);
  CHECK(env.Get32(a + 0x50684) == 0xCAFEF00D);
  CHECK(env.Get32(e + 0x50684) == 0xCAFEF00D);
  env.Put32(e + 0x2FFFFC, 0xDEADBEEF);
  CHECK(env.Get32(a + 0x2FFFFC) == 0xDEADBEEF);
  CHECK(env.Get32(c + 0x2FFFFC) == 0xDEADBEEF);

  env.Call(__imp__MmFreePhysicalMemory, {0, a});
  CHECK(BytesInUse() == before);
  // freed physical memory stays committed and readable from all views
  g_watch_begin = uintptr_t(env.mem.base);
  g_watch_end = uintptr_t(env.mem.base + GuestMemory::kSize);
  void* handler = AddVectoredExceptionHandler(1, &CommitOnFault);
  REQUIRE(handler != nullptr);
  g_fault = 0;
  CHECK(env.Get32(a + 0x50684) == 0xCAFEF00D);  // contents untouched by the free
  CHECK(env.Get32(e + 0x2FFFFC) == 0xDEADBEEF);
  CHECK(g_fault == 0);
  RemoveVectoredExceptionHandler(handler);
  // reallocated (same place, from the top): zero from all views
  const uint32_t b = env.Call(__imp__MmAllocatePhysicalMemoryEx,
                              {0, 0x300000, 0x20000404, 0, 0xFFFFFFFF, 0x80});
  REQUIRE(b == a);
  CHECK(env.Get32(c + 0x50684) == 0);
  CHECK(env.Get32(e + 0x2FFFFC) == 0);
  CHECK(BytesInUse() == before + 0x300000);
}

TEST_CASE("memory: CanonicalPhysicalAddress maps 0xC/0xE to 0xA (0xE with the offset), the rest unchanged") {
  CHECK(CanonicalPhysicalAddress(0xC0000000) == 0xA0000000);
  CHECK(CanonicalPhysicalAddress(0xDFFFFFFF) == 0xBFFFFFFF);
  // 0xE view: physical = address & 0x1FFFFFFF + 0x1000
  CHECK(CanonicalPhysicalAddress(0xE0000000) == 0xA0001000);
  CHECK(CanonicalPhysicalAddress(0xFFFFEFFF) == 0xBFFFFFFF);
  CHECK(CanonicalPhysicalAddress(0xE1234567) == 0xA1235567);
  // last page of 0xE: physical 0x20000000, outside the physical heap (no block)
  CHECK(CanonicalPhysicalAddress(0xFFFFF000) == 0xC0000000);
  CHECK(CanonicalPhysicalAddress(0xFFFFFFFF) == 0xC0000FFF);
  CHECK(CanonicalPhysicalAddress(0xA1234567) == 0xA1234567);
  CHECK(CanonicalPhysicalAddress(0xBFFFFFFF) == 0xBFFFFFFF);
  CHECK(CanonicalPhysicalAddress(0x40001000) == 0x40001000);
  CHECK(CanonicalPhysicalAddress(0x82000000) == 0x82000000);
  CHECK(CanonicalPhysicalAddress(0) == 0);
}

TEST_CASE("memory: MmFreePhysicalMemory from 0xC and from 0xE frees the physical block") {
  Env env;
  const uint64_t before = BytesInUse();
  for (const uint32_t mirror : {0xC0000000u, 0xE0000000u}) {
    INFO("mirror " << std::hex << mirror);
    const uint32_t a = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x20000, kPageReadWrite, 0, 0xFFFFFFFF, 0});
    REQUIRE((a & 0xE0000000) == 0xE0000000);  // 4 KB pages: 0xE view
    REQUIRE(BytesInUse() == before + 0x20000);
    // address of the same physical in the mirror (from 0xE: physical - 0x1000)
    const uint32_t phys = env.Call(__imp__MmGetPhysicalAddress, {a});
    REQUIRE(phys >= 0x1000);
    const uint32_t m = mirror == 0xE0000000u ? (mirror | phys) - 0x1000 : mirror | phys;
    env.Call(__imp__MmFreePhysicalMemory, {0, m});
    CHECK(BytesInUse() == before);
    CHECK(env.Call(__imp__MmQueryAllocationSize, {a}) == 0);
    // reallocated at the same place: the space really became free again
    const uint32_t b = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x20000, kPageReadWrite, 0, 0xFFFFFFFF, 0});
    CHECK(b == a);
    env.Call(__imp__MmFreePhysicalMemory, {0, b});
    CHECK(BytesInUse() == before);
  }
}

TEST_CASE("memory: MmQueryAllocationSize, MmQueryAddressProtect and NtQueryVirtualMemory equal on the three mirrors") {
  Env env;
  const uint32_t a =
      env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x30000, kPageReadWrite | kPageNoCache, 0, 0xFFFFFFFF, 0});
  REQUIRE(a != 0);
  // address of physical p + 0x11234 in each view (from 0xE: physical - 0x1000)
  const uint32_t phys = env.Call(__imp__MmGetPhysicalAddress, {a});
  REQUIRE(phys >= 0x1000);
  const uint32_t probe = phys + 0x11234;
  const auto in = [](uint32_t mirror, uint32_t p) { return mirror == 0xE0000000u ? mirror + p - 0x1000 : mirror + p; };
  const uint32_t size_a = env.Call(__imp__MmQueryAllocationSize, {0xA0000000u + probe});
  const uint32_t prot_a = env.Call(__imp__MmQueryAddressProtect, {0xA0000000u + probe});
  CHECK(size_a == 0x30000);
  CHECK(prot_a == (kPageReadWrite | kPageNoCache));
  REQUIRE(env.QueryVirtual(0xA0000000u + probe) == kStatusSuccess);
  uint32_t info_a[7];
  for (int i = 0; i < 7; ++i) info_a[i] = env.Info(i);
  CHECK(info_a[1] == (0xA0000000u | phys));  // allocation_base, in the queried view
  for (const uint32_t mirror : {0xC0000000u, 0xE0000000u}) {
    INFO("mirror " << std::hex << mirror);
    CHECK(env.Call(__imp__MmQueryAllocationSize, {in(mirror, probe)}) == size_a);
    CHECK(env.Call(__imp__MmQueryAddressProtect, {in(mirror, probe)}) == prot_a);
    REQUIRE(env.QueryVirtual(in(mirror, probe)) == kStatusSuccess);
    // base_address and allocation_base in the queried view, the rest equal
    CHECK(env.Info(0) == in(mirror, info_a[0] & 0x1FFFFFFF));
    CHECK(env.Info(1) == in(mirror, info_a[1] & 0x1FFFFFFF));
    for (int i = 2; i < 7; ++i) CHECK(env.Info(i) == info_a[i]);
  }
  env.Call(__imp__MmFreePhysicalMemory, {0, a});
  for (const uint32_t mirror : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    CHECK(env.Call(__imp__MmQueryAllocationSize, {in(mirror, probe)}) == 0);
    CHECK(env.Call(__imp__MmQueryAddressProtect, {in(mirror, probe)}) == 0);
  }
}

TEST_CASE("memory: NtFreeVirtualMemory on the process heap's arena rejected, the heap stays usable") {
  Env env;
  const uint32_t p = env.Call(__imp__RtlAllocateHeap, {0, 0, 0x100});
  REQUIRE(p != 0);
  REQUIRE(env.QueryVirtual(p) == kStatusSuccess);
  const uint32_t arena = env.Info(1);  // allocation_base
  REQUIRE(arena != 0);
  const uint64_t before = BytesInUse();
  CHECK(env.FreeVirtual(arena, 0, kMemRelease).status == kStatusInvalidParameter);
  CHECK(env.FreeVirtual(p & ~0xFFFu, 0x1000, kMemDecommit).status == kStatusInvalidParameter);
  CHECK(env.FreeVirtual(arena, 0, kMemRelease).status == kStatusInvalidParameter);
  CHECK(BytesInUse() == before);
  REQUIRE(env.QueryVirtual(p) == kStatusSuccess);
  CHECK(env.Info(4) == kMemCommit);  // state
  const uint32_t q = env.Call(__imp__RtlAllocateHeap, {0, 0x8, 0x1000});
  REQUIRE(q != 0);
  env.Put32(q + 0xFFC, 0x12345678);  // pages still committed
  CHECK(env.Get32(q + 0xFFC) == 0x12345678);
  env.Put32(p, 0xCAFEBABE);
  CHECK(env.Get32(p) == 0xCAFEBABE);
  CHECK(env.Call(__imp__RtlFreeHeap, {0, 0, q}) == 1);
  CHECK(env.Call(__imp__RtlFreeHeap, {0, 0, p}) == 1);
  // normal allocations of the same heap are freed as before
  const auto r = env.AllocVirtual(0, 0x10000, kMemReserve | kMemCommit | kMemLargePages);
  REQUIRE(r.status == kStatusSuccess);
  CHECK(env.FreeVirtual(r.base, 0, kMemRelease).status == kStatusSuccess);
}

TEST_CASE("memory: NtFreeVirtualMemory and NtQueryVirtualMemory on a physical block from the three mirrors") {
  Env env;
  const uint64_t before = BytesInUse();
  for (const uint32_t mirror : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    INFO("mirror " << std::hex << mirror);
    const uint32_t a = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x10000, kPageReadWrite, 0, 0xFFFFFFFF, 0});
    REQUIRE(a != 0);
    // address of the same physical in the mirror (from 0xE: physical - 0x1000)
    const uint32_t phys = env.Call(__imp__MmGetPhysicalAddress, {a});
    REQUIRE(phys >= 0x1000);
    const uint32_t m = mirror == 0xE0000000u ? (mirror | phys) - 0x1000 : mirror | phys;
    const auto r = env.FreeVirtual(m, 0, kMemRelease);
    CHECK(r.status == kStatusSuccess);
    CHECK(r.base == m);
    CHECK(r.size == 0x10000);
    CHECK(BytesInUse() == before);
    CHECK(env.QueryVirtual(m) == kStatusInvalidParameter);  // no longer allocated
  }
}

TEST_CASE("memory: ~400 MiB physical in mixed 64 KB and 64 MiB blocks, then budget") {
  Env env;
  constexpr uint32_t kLarge = kPageReadWrite | kMemLargePages;
  std::vector<uint32_t> blocks;
  // 5 x 64 MiB + 1280 x 64 KB = 400 MiB, alternating, 64 KB-aligned
  for (int i = 0; i < 5; ++i) {
    const uint32_t big = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 64 * kMiB, kLarge, 0, 0xFFFFFFFF, 0x10000});
    REQUIRE(big != 0);
    CHECK(big % 0x10000 == 0);
    blocks.push_back(big);
    for (int j = 0; j < 256; ++j) {
      const uint32_t small = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x10000, kLarge, 0, 0xFFFFFFFF, 0x10000});
      REQUIRE(small != 0);
      CHECK(small % 0x10000 == 0);
      blocks.push_back(small);
    }
  }
  for (uint32_t b : blocks) {
    REQUIRE(b >= kPhysicalHeapBegin);
    REQUIRE(uint64_t(b) + 0x10000 <= kPhysicalHeapEnd);
  }
  CHECK(BytesInUse() == 400 * kMiB);

  const uint32_t st = env.scratch + 0x400;
  env.Put32(st, 104);
  REQUIRE(env.Call(__imp__MmQueryStatistics, {st}) == kStatusSuccess);
  CHECK(env.Get32(st + 12) == 0x20000 - (400 * kMiB) / 0x1000);

  // what the statistics say is free really allocates; beyond the budget -> 0
  const uint32_t rest = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 100 * kMiB, kLarge, 0, 0xFFFFFFFF, 0x10000});
  CHECK(rest != 0);
  CHECK(env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 16 * kMiB, kLarge, 0, 0xFFFFFFFF, 0x10000}) == 0);
  CHECK(BytesInUse() == 500 * kMiB);
  env.Put32(st, 104);
  REQUIRE(env.Call(__imp__MmQueryStatistics, {st}) == kStatusSuccess);
  CHECK(env.Get32(st + 12) == 0x20000 - (500 * kMiB) / 0x1000);

  // physical range [min, max]: the block lies inside it
  env.Call(__imp__MmFreePhysicalMemory, {0, rest});
  const uint32_t ranged =
      env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x10000, kPageReadWrite, 0x00100000, 0x001FFFFF, 0});
  REQUIRE(ranged != 0);
  const uint32_t ranged_phys = env.Call(__imp__MmGetPhysicalAddress, {ranged});
  CHECK(ranged_phys >= 0x00100000);
  CHECK(ranged_phys + 0x10000 - 1 <= 0x001FFFFF);
  env.Call(__imp__MmFreePhysicalMemory, {0, ranged});

  for (uint32_t b : blocks) env.Call(__imp__MmFreePhysicalMemory, {0, b});
  CHECK(BytesInUse() == 0);
}

TEST_CASE("memory: RtlAllocateHeap, RtlSizeHeap, RtlReAllocateHeap preserves contents, RtlFreeHeap") {
  Env env;
  const uint32_t p = env.Call(__imp__RtlAllocateHeap, {0, 0x8 /*HEAP_ZERO_MEMORY*/, 100});
  REQUIRE(p != 0);
  CHECK(p >= kVirtualHeapBegin);
  CHECK(p < kVirtualHeapEnd);
  CHECK(env.Call(__imp__RtlSizeHeap, {0, 0, p}) == 100);
  for (uint32_t i = 0; i < 100; ++i) REQUIRE(env.mem.base[p + i] == 0);
  for (uint32_t i = 0; i < 100; ++i) env.mem.base[p + i] = uint8_t(i + 1);
  CHECK(BytesInUse() == 100);

  // another block right after forces the move
  const uint32_t q = env.Call(__imp__RtlAllocateHeap, {0, 0, 64});
  REQUIRE(q != 0);
  const uint32_t p2 = env.Call(__imp__RtlReAllocateHeap, {0, 0x8, p, 100000});
  REQUIRE(p2 != 0);
  CHECK(env.Call(__imp__RtlSizeHeap, {0, 0, p2}) == 100000);
  for (uint32_t i = 0; i < 100; ++i) REQUIRE(env.mem.base[p2 + i] == uint8_t(i + 1));
  for (uint32_t i = 100; i < 100000; i += 997) REQUIRE(env.mem.base[p2 + i] == 0);  // extension zeroed
  CHECK(BytesInUse() == 100000 + 64);

  // shrinking: contents preserved
  const uint32_t p3 = env.Call(__imp__RtlReAllocateHeap, {0, 0, p2, 10});
  REQUIRE(p3 != 0);
  for (uint32_t i = 0; i < 10; ++i) REQUIRE(env.mem.base[p3 + i] == uint8_t(i + 1));
  CHECK(env.Call(__imp__RtlSizeHeap, {0, 0, p3}) == 10);

  CHECK(env.Call(__imp__RtlFreeHeap, {0, 0, p3}) == 1);
  CHECK(env.Call(__imp__RtlFreeHeap, {0, 0, q}) == 1);
  CHECK(env.Call(__imp__RtlFreeHeap, {0, 0, 0}) == 1);
  CHECK(env.Call(__imp__RtlSizeHeap, {0, 0, p3}) == 0);
  CHECK(BytesInUse() == 0);
}

TEST_CASE("memory: InitMemory links the process heap to RtlUnicodeStringToAnsiString/RtlFreeAnsiString") {
  Env env;
  const uint32_t s = env.scratch + 0x800;
  // UNICODE_STRING "Ab" (big-endian UTF-16) and an empty ANSI_STRING to allocate
  env.Put16(s + 0x40, u'A');
  env.Put16(s + 0x42, u'b');
  env.Put16(s, 4);
  env.Put16(s + 2, 6);
  env.Put32(s + 4, s + 0x40);
  env.Put32(s + 0x10, 0);
  env.Put32(s + 0x14, 0);
  REQUIRE(env.Call(__imp__RtlUnicodeStringToAnsiString, {s + 0x10, s, 1}) == kStatusSuccess);
  const uint32_t buf = env.Get32(s + 0x14);
  REQUIRE(buf != 0);
  CHECK(buf >= kVirtualHeapBegin);
  CHECK(buf < kVirtualHeapEnd);
  CHECK(std::string(reinterpret_cast<char*>(env.mem.base + buf)) == "Ab");
  CHECK(env.Call(__imp__RtlSizeHeap, {0, 0, buf}) >= 3);
  CHECK(BytesInUse() > 0);

  env.Call(__imp__RtlFreeAnsiString, {s + 0x10});
  CHECK(env.Get32(s + 0x14) == 0);
  CHECK(env.Call(__imp__RtlSizeHeap, {0, 0, buf}) == 0);
  CHECK(BytesInUse() == 0);
}
