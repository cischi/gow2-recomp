// Operating system abstraction: the only place in the runtime that calls
// OS APIs (VirtualAlloc, exception handling, ...).
#pragma once

#include <cstdint>
#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace plat {

// Reserves and commits `size` readable/writable bytes, at `preferred_address`
// if free, otherwise wherever possible. nullptr on failure. Zeroed memory.
void* ReserveCommit(uint64_t preferred_address, uint64_t size);
// Reserves `size` bytes of address space without committing them (every access
// fails until Commit makes them writable), at `preferred` if free,
// otherwise wherever possible. nullptr on failure.
void* Reserve(uint64_t preferred, uint64_t size);
// Commits readable/writable pages inside a reservation (p and size already
// page-aligned). Pages committed for the first time, or again after
// Decommit, are zeroed; already committed ones stay intact.
bool Commit(void* p, uint64_t size);
// Decommits pages of a reservation: they become inaccessible again (and zeroed
// at the next Commit). The reservation remains.
bool Decommit(void* p, uint64_t size);
// Releases a block obtained from ReserveCommit or Reserve (nullptr allowed).
void Release(void* p, uint64_t size);

// ---- shared memory and views (mirrors of the guest physical memory) --------
// An anonymous section (pagefile) is mapped as several views inside a space
// reserved as placeholders: every view sees the same bytes. Requires Windows 10
// 1803+ (VirtualAlloc2/MapViewOfFile3, loaded at runtime from kernelbase.dll).

// Anonymous section of `size` bytes with only reserved pages: they are committed
// with Commit on any view (zeroed, visible from all), without decommit.
// nullptr on failure.
void* CreateSharedMemory(uint64_t size);
void CloseSharedMemory(void* section);  // nullptr allowed
// Reserves `size` bytes as a placeholder, at `preferred` if free, otherwise
// wherever possible. nullptr on failure or if the system has no placeholders.
void* ReservePlaceholder(uint64_t preferred, uint64_t size);
// Splits [p, p+size), inside a placeholder, into a placeholder of its own.
bool SplitPlaceholder(void* p, uint64_t size);
// Replaces the placeholder [p, p+size) with a normal reservation (Commit,
// Decommit and Release as for Reserve).
bool PlaceholderToReserve(void* p, uint64_t size);
// Replaces the placeholder [p, p+size) with a readable/writable view
// of the section from `offset` (multiple of 64 KB).
bool MapViewAt(void* section, void* p, uint64_t offset, uint64_t size);
// Unmaps a MapViewAt view (the space becomes free again).
void UnmapView(void* p);
// Whole contents of the file; nullopt if it does not exist or cannot be read.
std::optional<std::vector<uint8_t>> ReadWholeFile(const std::filesystem::path&);

// Console of a /SUBSYSTEM:WINDOWS process. If stderr is already a valid handle (pipe or file) nothing is done;
// otherwise attach to the parent console, or with `want_console` allocate a new one, and reopen
// stdout/stderr on it (C stdio and iostreams). Safe to call more than once: call early with false, then
// again once the setting is known.
void SetupConsole(bool want_console);
// True if a stderr/console is available (see SetupConsole); when false errors need a message box.
bool OutputVisible();
// Modal error box (UTF-8 text); for use when OutputVisible() is false.
void ShowErrorBox(std::string_view utf8_message);

// Full path of the current executable (empty if it cannot be obtained).
std::filesystem::path ExecutablePath();

// Makes `size` bytes from `p` (page-aligned, committed with
// ReserveCommit or Commit) inaccessible: every access raises an access violation.
// false on failure (or p == nullptr).
bool ProtectNoAccess(void* p, uint64_t size);
// Opposite of ProtectNoAccess: committed pages readable/writable again.
bool ProtectReadWrite(void* p, uint64_t size);
// Committed pages read-only: every write raises an access violation.
bool ProtectReadOnly(void* p, uint64_t size);
// true if the page containing `p` is committed and writable right now (to
// tell a fault on a page just reopened by another thread apart from a
// real crash). Does not allocate.
bool IsWritable(const void* p);

// Handler for write access violations (AddVectoredExceptionHandler(1, ...)):
// receives the written address; true = handled (the instruction is re-run),
// false = the exception continues to the crash filter. Runs before the
// crash filter, on the faulting thread: it must not allocate. Other
// exceptions (reads, execution, different codes) do not reach it.
using WriteFaultHandler = bool (*)(uintptr_t data_address);
// Installs (or replaces; nullptr disables it) the handler. The vectored
// handler is registered only once.
void InstallWriteFaultHandler(WriteFaultHandler);

// Data of an unhandled exception passed to the crash handler.
struct CrashInfo {
  uintptr_t host_pc;      // address of the instruction that faulted
  uint32_t code;          // exception code (0xC0000005 = access violation)
  bool has_data_address;  // access violations only
  uintptr_t data_address; // address read/written that caused the fault
  uintptr_t stack_pointer = 0;  // RSP of the thread at the fault (0 if unknown)
  uintptr_t stack_base = 0;     // end (exclusive) of the thread's host stack
};
// Called on an unhandled exception; after it returns the process terminates
// as usual. Runs in the exception filter: it must not allocate nor
// take locks that the crashing thread might hold.
using CrashCallback = void (*)(const CrashInfo&);
void InstallCrashHandler(CrashCallback);

// Process is per-monitor DPI aware v2: window sizes are in physical
// pixels. To be called at startup, before creating windows; if DPI
// awareness is already set it does nothing.
void EnablePerMonitorDpiAwareness();

// Resolution of the primary monitor in physical pixels (after EnablePerMonitorDpiAwareness).
std::pair<uint32_t, uint32_t> PrimaryMonitorSize();

// ---- threads ---------------------------------------------------------------

// Name of the current thread in the debugger and in tools (ASCII).
void SetCurrentThreadName(std::string_view name);
// Terminates the current thread without unwinding the stack (like ExTerminateThread
// on the console). Objects on the thread's stack are not destroyed.
[[noreturn]] void ExitCurrentThread(uint32_t code);
// Approximate host priority of a thread (native handle of
// std::thread::native_handle): -2 lowest .. 0 normal .. 2 highest.
// false on failure.
bool SetThreadPriority(void* native_handle, int level);

// 50 MHz performance counter (KeQueryPerformanceFrequency frequency on the
// console): rescaled QueryPerformanceCounter, monotonic.
uint64_t Ticks50MHz();

// Suspends the current thread for about `microseconds` with a high-resolution
// timer (sub-millisecond precision; Windows Sleep rounds to ~15.6 ms).
// Used by the 60 Hz vblank of the fake GPU.
void SleepPrecise(uint32_t microseconds);

// ---- audio output ----------------------------------------------------------
// Audio frame: 6 channels x 256 float samples at 48 kHz, interleaved on the host side.
inline constexpr uint32_t kAudioChannels = 6, kAudioSampleRate = 48000, kAudioFrameSamples = 256,
                          kAudioMaxQueued = 64;
inline constexpr size_t kAudioFrameFloats = kAudioChannels * kAudioFrameSamples;  // 1536
enum class AudioSinkKind { kNull, kXAudio2 };
class AudioOut {
 public:
  using BufferDone = std::function<void()>;  // once per consumed buffer, from a sink thread
  // Stops the sink thread; after it returns on_done is no longer called
  // (do not destroy from inside on_done).
  virtual ~AudioOut() = default;
  // false if the queue already has kAudioMaxQueued buffers.
  virtual bool Submit(std::span<const float, kAudioFrameFloats> interleaved) = 0;
  virtual void SetMuted(bool) = 0;
  virtual AudioSinkKind Kind() const = 0;
};
// Null sink: consumes one buffer every 5333 us (187.5/s, SleepPrecise with absolute
// deadlines, no drift) if the queue is not empty. The accumulated delay is
// limited to 100 ms: after a longer stall the excess ticks are lost
// (no catch-up burst), so the average is not exactly 187.5/s.
std::unique_ptr<AudioOut> CreateNullAudioOut(AudioOut::BufferDone on_done);
// XAudio2 2.9 output (model: src/xenia/apu/xaudio2/xaudio2_audio_driver.cc
// of Xenia @ 95a5c3ee, BSD). Loads xaudio2_9.dll (XAudio2Create via
// GetProcAddress), a dedicated MTA thread for COM that creates and destroys the
// objects, float32 source voice with 6 channels, 48 kHz, 5.1 mask (0x3F), at
// most 64 queued buffers; OnBufferEnd -> on_done (on the XAudio2 engine
// thread: on_done must not block). Submit can be called from any
// thread, even from on_done; no internal lock is held during
// calls to XAudio2 or to on_done. nullptr + *hr_out (if not null, always
// != 0) if the DLL, engine or mastering voice are missing (no endpoint).
// IXAudio2EngineCallback::OnCriticalError (headphones unplugged) writes once
// `[audio] xaudio2 critical error (hr=0x%08X), using the null sink` and the same
// object switches to the internal null pacing: the queued and new buffers
// are consumed one every 5333 us, so the on_done calls do not stop. SetMuted ->
// SetVolume(0/1) on the mastering voice.
std::unique_ptr<AudioOut> CreateXAudio2AudioOut(AudioOut::BufferDone on_done, uint32_t* hr_out);
// Tests only: simulates IXAudio2EngineCallback::OnCriticalError on a sink from
// CreateXAudio2AudioOut (no effect on other sinks).
void SimulateAudioCriticalError(AudioOut&);
// Tests only: the next SubmitSourceBuffer of a sink from CreateXAudio2AudioOut
// fails (XAUDIO2_E_DEVICE_INVALIDATED): the sink switches to null pacing as
// for a critical error and the buffer stays accepted.
void SimulateAudioSubmitFailure(AudioOut&);

// ---- game window -----------------------------------------------------------
// The renderer (runtime/gpu) receives the window only as an opaque handle
// (NativeHandle); swap chain and presentation are its own.
struct Window;
// Same values as settings::WindowMode (platform does not depend on settings).
enum class WindowMode { kWindowed, kBorderless, kFullscreen };
struct WindowOptions {
  // Client area in windowed mode (also when coming back from Alt-Enter), in physical pixels if
  // the process is DPI aware (EnablePerMonitorDpiAwareness).
  uint32_t width = 1280, height = 720;
  WindowMode mode = WindowMode::kWindowed;
  bool resizable = false;  // windowed only: draggable border and maximize
};
// Creates the window on a dedicated host thread, which runs its message pump,
// visible if `visible`. windowed: client area width x height, resizable
// if `resizable`. borderless and fullscreen: WS_POPUP as large as the monitor
// of the window (exclusive fullscreen is activated by the presenter), cursor
// hidden over the client area when the window has focus. DPI change
// (WM_DPICHANGED) in windowed mode: if resizable the rectangle proposed
// by Windows is accepted, otherwise the client area stays the same in physical pixels. Alt-Enter
// toggles between windowed and the configured fullscreen mode (borderless if `mode` is windowed), restoring
// the windowed position and size. WM_CLOSE (close button, Alt-F4,
// RequestClose) calls on_close only once on the window thread and does not
// destroy it: the caller decides (gow2 writes the summary and exits).
// Returns after creation; nullptr on failure.
Window* CreateGameWindow(std::string_view title, const WindowOptions& options, bool visible,
                         std::function<void()> on_close);
void* NativeHandle(Window*);  // HWND

// Refresh rate and VRR of the window's monitor (nullptr = primary monitor).
struct DisplaySync {
  uint32_t refresh_hz = 0;  // 0 = unknown
  std::optional<bool> vrr;  // NvAPI_Disp_GetVRRInfo.bIsVRREnabled; nullopt = not asked or NVAPI unavailable
  std::string note;         // with vrr nullopt and nvapi asked: reason
  uint32_t nvapi_us = 0;    // duration of the NVAPI part
};
DisplaySync QueryDisplaySync(Window*, bool nvapi);
// Current client area (width, height), updated by WM_SIZE; 0x0 if
// minimized. Lock-free read, from any thread.
std::pair<uint32_t, uint32_t> ClientSize(Window*);
// Current mode (changes with Alt-Enter); kWindowed with a null Window*.
WindowMode CurrentWindowMode(Window*);
// Exclusive fullscreen (presenter, on every Present): `active` is the current
// DXGI state; returns true if it should be activated (fullscreen mode and focus). Alt-Enter
// during exclusive mode switches to kWindowed immediately but the windowed style and position are
// applied only when the presenter reports active = false (leaving
// exclusive mode restores the previous rectangle). false with a null Window*.
bool ExclusiveFullscreenWanted(Window*, bool active);
// Applies new options (F1 overlay): posts a message and does not wait. The window
// thread updates the resizable border and the Alt-Enter target, then switches
// to the mode like Alt-Enter (from exclusive fullscreen: windowed only after the
// presenter leaves); in windowed mode the client area becomes width x height.
void ApplyWindowOptions(Window*, const WindowOptions&);
// Message hook (F1 overlay), called on the window thread for every message
// except WM_NCCREATE, WM_CLOSE, WM_DESTROY, private messages (WM_APP+) and Alt-Enter. A
// value = message consumed with that LRESULT; a consumed keyboard message leaves
// the key released for IsKeyDown. Empty = no hook; null Window*: nothing. The hook runs
// under a window mutex: SetMessageHook returns after any call in progress, and the
// hook must not call SetMessageHook/ApplyWindowOptions.
using MessageHook = std::function<std::optional<intptr_t>(uint32_t msg, uintptr_t wp, intptr_t lp)>;
void SetMessageHook(Window*, MessageHook);
// Mouse capture on the window (true) or release, done by the window thread outside the hook
// (SetCapture only works from the window thread and immediately sends WM_CAPTURECHANGED). Does not wait; also from
// the hook. Null Window*: nothing.
void SetMouseCapture(Window*, bool capture);
// Posts WM_CLOSE to the window (for tests). Does not wait.
void RequestClose(Window*);
// Destroys the window, stops the message pump and waits for the thread.
// nullptr allowed. Does not call on_close.
void DestroyGameWindow(Window*);

// Window keyboard: the window thread updates atomics from
// WM_KEYDOWN/WM_KEYUP (also WM_SYS*) and WM_SETFOCUS/WM_KILLFOCUS (losing
// focus releases all keys). Lock-free read, from any thread.
// true if the virtual key (VK_*) is pressed; false without focus or with a null Window*.
bool IsKeyDown(Window*, uint8_t virtual_key);
bool HasFocus(Window*);  // false with a null Window*
// All keys released (F1 overlay on opening); from any thread.
void ReleaseAllKeys(Window*);

// ---- XInput pad ------------------------------------------------------------
// xinput1_4.dll loaded at runtime (LoadLibraryW + GetProcAddress, once).
// State in Windows format (XINPUT_GAMEPAD): the button bits are identical
// to the console's.
struct PadState {
  uint16_t buttons;
  uint8_t lt, rt;
  int16_t lx, ly, rx, ry;
};
bool XInputAvailable();  // false if the DLL or the functions are missing
// User 0..3; nullopt = not connected (or DLL missing). XInputGetState is
// slow for disconnected pads: the caller should limit polling.
std::optional<PadState> XInputGetState(uint32_t user);
bool XInputSetVibration(uint32_t user, uint16_t left, uint16_t right);  // false = not connected

}  // namespace plat
