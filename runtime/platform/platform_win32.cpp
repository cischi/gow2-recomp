// Win32 implementation of platform.h: the only runtime file that uses windows.h.
#include "platform/platform.h"

#include <windows.h>
#include <process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <io.h>
#include <iostream>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace plat {

void* ReserveCommit(uint64_t preferred_address, uint64_t size) {
  constexpr DWORD kType = MEM_RESERVE | MEM_COMMIT;
  void* p = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(preferred_address)), SIZE_T(size), kType,
                         PAGE_READWRITE);
  if (!p) p = VirtualAlloc(nullptr, SIZE_T(size), kType, PAGE_READWRITE);
  return p;
}

void* Reserve(uint64_t preferred, uint64_t size) {
  void* p = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(preferred)), SIZE_T(size), MEM_RESERVE, PAGE_NOACCESS);
  if (!p) p = VirtualAlloc(nullptr, SIZE_T(size), MEM_RESERVE, PAGE_NOACCESS);
  return p;
}

bool Commit(void* p, uint64_t size) {
  if (!p || size == 0) return false;
  return VirtualAlloc(p, SIZE_T(size), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool Decommit(void* p, uint64_t size) {
  if (!p || size == 0) return false;
  return VirtualFree(p, SIZE_T(size), MEM_DECOMMIT) != 0;
}

void Release(void* p, uint64_t /*size*/) {
  // MEM_RELEASE requires size 0 and frees the whole reservation.
  if (p) VirtualFree(p, 0, MEM_RELEASE);
}

namespace {
// VirtualAlloc2 and MapViewOfFile3 live in kernelbase.dll (Windows 10 1803+)
// and their import lib is onecore.lib, which does not mix with kernel32.lib:
// they are resolved at runtime.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);
struct PlaceholderApi {
  VirtualAlloc2Fn virtual_alloc2 = nullptr;
  MapViewOfFile3Fn map_view3 = nullptr;
  PlaceholderApi() {
    if (HMODULE k = GetModuleHandleW(L"kernelbase.dll")) {
      virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(k, "VirtualAlloc2"));
      map_view3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(k, "MapViewOfFile3"));
    }
  }
  bool ok() const { return virtual_alloc2 && map_view3; }
};
const PlaceholderApi& Api() {
  static const PlaceholderApi api;
  return api;
}
}  // namespace

void* CreateSharedMemory(uint64_t size) {
  // SEC_RESERVE: no pagefile charge until the pages are committed.
  return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, DWORD(size >> 32),
                            DWORD(size), nullptr);
}

void CloseSharedMemory(void* section) {
  if (section) CloseHandle(section);
}

void* ReservePlaceholder(uint64_t preferred, uint64_t size) {
  const auto& api = Api();
  if (!api.ok()) return nullptr;
  constexpr ULONG kType = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
  void* p = api.virtual_alloc2(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(preferred)), SIZE_T(size),
                               kType, PAGE_NOACCESS, nullptr, 0);
  if (!p) p = api.virtual_alloc2(GetCurrentProcess(), nullptr, SIZE_T(size), kType, PAGE_NOACCESS, nullptr, 0);
  return p;
}

bool SplitPlaceholder(void* p, uint64_t size) {
  return p && VirtualFree(p, SIZE_T(size), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
}

bool PlaceholderToReserve(void* p, uint64_t size) {
  const auto& api = Api();
  if (!p || !api.ok()) return false;
  return api.virtual_alloc2(GetCurrentProcess(), p, SIZE_T(size), MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                            PAGE_NOACCESS, nullptr, 0) == p;
}

bool MapViewAt(void* section, void* p, uint64_t offset, uint64_t size) {
  const auto& api = Api();
  if (!section || !p || !api.ok()) return false;
  return api.map_view3(section, GetCurrentProcess(), p, offset, SIZE_T(size), MEM_REPLACE_PLACEHOLDER,
                       PAGE_READWRITE, nullptr, 0) == p;
}

void UnmapView(void* p) {
  if (p) UnmapViewOfFile(p);
}

std::optional<std::vector<uint8_t>> ReadWholeFile(const std::filesystem::path& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  std::optional<std::vector<uint8_t>> result;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(h, &size) && size.QuadPart >= 0) {
    std::vector<uint8_t> data(size_t(size.QuadPart));
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
      const DWORD chunk = DWORD(std::min<size_t>(data.size() - done, 1u << 30));
      DWORD got = 0;
      if (!ReadFile(h, data.data() + done, chunk, &got, nullptr) || got == 0) {
        ok = false;
        break;
      }
      done += got;
    }
    if (ok) result = std::move(data);
  }
  CloseHandle(h);
  return result;
}

namespace {
std::atomic<CrashCallback> g_crash_callback{nullptr};

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
  if (auto cb = g_crash_callback.load()) {
    const auto* rec = info->ExceptionRecord;
    CrashInfo crash{reinterpret_cast<uintptr_t>(rec->ExceptionAddress), uint32_t(rec->ExceptionCode), false, 0};
    // Access violation: ExceptionInformation[0] = read/write, [1] = address.
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
      crash.has_data_address = true;
      crash.data_address = uintptr_t(rec->ExceptionInformation[1]);
    }
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    crash.stack_pointer = uintptr_t(info->ContextRecord->Rsp);
    crash.stack_base = uintptr_t(high);
    cb(crash);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

bool ProtectNoAccess(void* p, uint64_t size) {
  if (!p || size == 0) return false;
  DWORD old = 0;
  return VirtualProtect(p, SIZE_T(size), PAGE_NOACCESS, &old) != 0;
}

bool ProtectReadWrite(void* p, uint64_t size) {
  if (!p || size == 0) return false;
  DWORD old = 0;
  return VirtualProtect(p, SIZE_T(size), PAGE_READWRITE, &old) != 0;
}

bool ProtectReadOnly(void* p, uint64_t size) {
  if (!p || size == 0) return false;
  DWORD old = 0;
  return VirtualProtect(p, SIZE_T(size), PAGE_READONLY, &old) != 0;
}

bool IsWritable(const void* p) {
  MEMORY_BASIC_INFORMATION info{};
  if (!p || VirtualQuery(p, &info, sizeof(info)) != sizeof(info)) return false;
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
  constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (info.Protect & kWritable) != 0;
}

namespace {
std::atomic<WriteFaultHandler> g_write_fault_handler{nullptr};

LONG CALLBACK WriteFaultVectoredHandler(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;
  // ExceptionInformation[0]: 0 read, 1 write, 8 execute (DEP).
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 ||
      rec->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;
  const WriteFaultHandler handler = g_write_fault_handler.load(std::memory_order_acquire);
  if (handler && handler(uintptr_t(rec->ExceptionInformation[1]))) return EXCEPTION_CONTINUE_EXECUTION;
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallWriteFaultHandler(WriteFaultHandler handler) {
  g_write_fault_handler.store(handler, std::memory_order_release);
  // First in the chain: before any other vectored handler already registered
  // and before the unhandled-exception filter (crash handler).
  static void* const registered = AddVectoredExceptionHandler(1, &WriteFaultVectoredHandler);
  (void)registered;
}

std::filesystem::path ExecutablePath() {
  std::wstring buf(MAX_PATH, wchar_t{});
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return std::filesystem::path(buf);
    }
    buf.resize(buf.size() * 2);  // truncated: retry with a larger buffer
  }
}

void InstallCrashHandler(CrashCallback cb) {
  g_crash_callback.store(cb);
  SetUnhandledExceptionFilter(cb ? &UnhandledFilter : nullptr);
}

void EnablePerMonitorDpiAwareness() {
  // Fails if already set (manifest or earlier call): that is fine.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

std::pair<uint32_t, uint32_t> PrimaryMonitorSize() {
  return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

void SetCurrentThreadName(std::string_view name) {
  std::wstring wide(name.begin(), name.end());  // ASCII names
  SetThreadDescription(GetCurrentThread(), wide.c_str());
}

void ExitCurrentThread(uint32_t code) {
  // std::thread threads start with _beginthreadex: _endthreadex also frees
  // the thread's CRT state.
  _endthreadex(code);
  ExitThread(code);  // not reached
}

bool SetThreadPriority(void* native_handle, int level) {
  static constexpr int kLevels[] = {THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
                                    THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST};
  if (!native_handle) return false;
  level = std::clamp(level, -2, 2);
  return ::SetThreadPriority(static_cast<HANDLE>(native_handle), kLevels[level + 2]) != 0;
}

uint64_t Ticks50MHz() {
  static const uint64_t freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? uint64_t(f.QuadPart) : uint64_t(1);
  }();
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  constexpr uint64_t kTarget = 50'000'000;
  // c * 50e6 / freq without 64-bit overflow: quotient and remainder separate.
  const uint64_t count = uint64_t(c.QuadPart);
  return (count / freq) * kTarget + (count % freq) * kTarget / freq;
}

void SleepPrecise(uint32_t microseconds) {
  // One timer per thread, created on first use (Windows 10 1803+: HIGH_RESOLUTION;
  // otherwise a normal timer, still better than Sleep with the default period).
  struct Timer {
    HANDLE h = nullptr;
    Timer() {
      h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~Timer() {
      if (h) CloseHandle(h);
    }
  };
  thread_local Timer timer;
  if (!timer.h) {
    Sleep(DWORD((microseconds + 999) / 1000));
    return;
  }
  LARGE_INTEGER due{};
  due.QuadPart = -int64_t(microseconds) * 10;  // relative, 100 ns units
  if (!SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
    Sleep(DWORD((microseconds + 999) / 1000));
    return;
  }
  WaitForSingleObject(timer.h, INFINITE);
}


// ---- game window ----------------------------------------------------

struct Window {
  HWND hwnd = nullptr;
  std::function<void()> on_close;
  std::atomic<bool> close_requested{false};
  std::thread thread;
  // Keyboard (window thread writes, anyone reads): one atomic per
  // virtual key, plus the focus.
  std::array<std::atomic<bool>, 256> keys{};
  std::atomic<bool> focus{false};
  // Client area (width << 32 | height) from WM_SIZE: a single atomic, never
  // half old and half new.
  std::atomic<uint64_t> client{0};
  std::atomic<WindowMode> mode{WindowMode::kWindowed};
  std::atomic<bool> exclusive{false};  // DXGI exclusive fullscreen active (from the presenter)
  // Window thread only.
  // Alt-Enter towards windowed during exclusive mode: the windowed style and position
  // are applied only after leaving (SetFullscreenState(FALSE) restores the
  // rectangle from before exclusive mode and would overwrite the windowed one).
  bool windowed_pending = false;
  WindowMode fullscreen_mode = WindowMode::kBorderless;  // Alt-Enter target from windowed
  DWORD windowed_style = 0;
  WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
  // Message hook (SetMessageHook) and options to apply (ApplyWindowOptions).
  std::mutex mutex;
  std::unique_ptr<const MessageHook> hook;
  std::optional<WindowOptions> pending_options;

  void ReleaseAllKeys() {
    for (auto& k : keys) k.store(false, std::memory_order_relaxed);
  }
};

namespace {

constexpr wchar_t kWindowClass[] = L"gow2_game_window";
// Private message: DestroyWindow must be called from the window thread.
constexpr UINT kMsgDestroy = WM_APP + 1;
// The presenter left exclusive fullscreen (ExclusiveFullscreenWanted).
constexpr UINT kMsgExclusiveLeft = WM_APP + 2;
// New options in Window::pending_options (ApplyWindowOptions).
constexpr UINT kMsgApplyOptions = WM_APP + 3;
// Mouse capture (SetMouseCapture): wParam 1 = take, 0 = release.
constexpr UINT kMsgMouseCapture = WM_APP + 4;
constexpr DWORD kWindowedBaseStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

// Size of the windowed window for a cw x ch client area in physical pixels
// at the given DPI (SetWindowPos; null pos: position unchanged).
void SetWindowedClientSize(HWND hwnd, DWORD style, LONG cw, LONG ch, UINT dpi, const POINT* pos) {
  RECT r{0, 0, cw, ch};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
  SetWindowPos(hwnd, nullptr, pos ? pos->x : 0, pos ? pos->y : 0, r.right - r.left, r.bottom - r.top,
               SWP_NOZORDER | SWP_NOACTIVATE | (pos ? 0 : SWP_NOMOVE));
}

// Windowed style and windowed_placement re-applied (window thread).
void RestoreWindowed(Window* w) {
  const HWND hwnd = w->hwnd;
  const DWORD visible = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)) & WS_VISIBLE;
  SetWindowLongPtrW(hwnd, GWL_STYLE, LONG_PTR(w->windowed_style | visible));
  WINDOWPLACEMENT wp = w->windowed_placement;
  // SetWindowPlacement applies showCmd: invisible stays invisible, minimized goes back to normal.
  wp.showCmd = !visible ? SW_HIDE : wp.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
  SetWindowPlacement(hwnd, &wp);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

// Switches from windowed (`old`) to fullscreen or back (window thread).
// The style is rewritten in full (plus WS_VISIBLE): a single state per
// mode, even with repeated Alt-Enter.
void ApplyWindowMode(Window* w, WindowMode mode, WindowMode old) {
  const HWND hwnd = w->hwnd;
  if (mode == old) return;
  const DWORD visible = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)) & WS_VISIBLE;
  if (old == WindowMode::kWindowed) GetWindowPlacement(hwnd, &w->windowed_placement);
  w->mode.store(mode);
  if (mode == WindowMode::kWindowed) {
    RestoreWindowed(w);
    return;
  }
  // Monitor not found: the primary one; information not read: the primary screen.
  MONITORINFO mi{sizeof(mi)};
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
    mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  SetWindowLongPtrW(hwnd, GWL_STYLE, LONG_PTR(WS_POPUP | visible));
  const RECT& r = mi.rcMonitor;
  SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
               SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

// Mode change from Alt-Enter and from ApplyWindowOptions (window thread). From
// exclusive fullscreen to windowed: windowed_pending, the presenter leaves, then kMsgExclusiveLeft.
void SwitchMode(Window* w, WindowMode target) {
  if (w->windowed_pending) {
    // Back to fullscreen before exclusive mode finished: nothing to re-apply.
    if (target == WindowMode::kWindowed) return;
    w->windowed_pending = false;
    w->mode.store(target);
    return;
  }
  const WindowMode old = w->mode.load();
  if (target == old) return;
  if (target == WindowMode::kWindowed && w->exclusive.load()) {
    w->windowed_pending = true;
    w->mode.store(WindowMode::kWindowed);
    return;
  }
  ApplyWindowMode(w, target, old);
}

// ApplyWindowOptions on the window thread.
void ApplyOptions(Window* w, const WindowOptions& o) {
  w->windowed_style = kWindowedBaseStyle | (o.resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
  if (o.mode != WindowMode::kWindowed) {
    w->fullscreen_mode = o.mode;
    SwitchMode(w, o.mode);
    return;
  }
  // Windowed rectangle for the new client area, from the current windowed position.
  const bool windowed_now = w->mode.load() == WindowMode::kWindowed && !w->windowed_pending;
  if (windowed_now) GetWindowPlacement(w->hwnd, &w->windowed_placement);
  RECT& r = w->windowed_placement.rcNormalPosition;
  RECT size{0, 0, LONG(o.width), LONG(o.height)};
  AdjustWindowRectExForDpi(&size, w->windowed_style, FALSE, 0, GetDpiForWindow(w->hwnd));
  r.right = r.left + (size.right - size.left);
  r.bottom = r.top + (size.bottom - size.top);
  if (w->windowed_placement.showCmd == SW_SHOWMAXIMIZED) w->windowed_placement.showCmd = SW_SHOWNORMAL;
  if (windowed_now) RestoreWindowed(w);
  else SwitchMode(w, WindowMode::kWindowed);
}

bool IsKeyboardMessage(UINT msg) {
  return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
}

LRESULT CALLBACK GameWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  const bool alt_enter = msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29));
  if (w && msg != WM_NCCREATE && msg != WM_CLOSE && msg != WM_DESTROY && msg < WM_APP && !alt_enter) {
    // Hook called under the mutex: SetMessageHook(w, {}) returns only once the hook has finished.
    std::optional<intptr_t> r;
    {
      std::lock_guard g(w->mutex);
      if (w->hook) r = (*w->hook)(msg, wp, lp);
    }
    if (r) {
      if (IsKeyboardMessage(msg) && wp < 256) w->keys[wp].store(false, std::memory_order_relaxed);
      return *r;
    }
  }
  switch (msg) {
    case WM_CLOSE:
      // No DefWindowProc (it would destroy the window): on_close decides.
      if (w && !w->close_requested.exchange(true) && w->on_close) w->on_close();
      return 0;
    case kMsgDestroy:
      DestroyWindow(hwnd);
      return 0;
    case kMsgApplyOptions:
      if (w) {
        std::optional<WindowOptions> o;
        {
          std::lock_guard g(w->mutex);
          o.swap(w->pending_options);
        }
        if (o) ApplyOptions(w, *o);
      }
      return 0;
    case kMsgMouseCapture:
      // Outside the hook: SetCapture/ReleaseCapture immediately send WM_CAPTURECHANGED to the window.
      if (wp) SetCapture(hwnd);
      else if (GetCapture() == hwnd) ReleaseCapture();
      return 0;
    case kMsgExclusiveLeft:
      if (w && w->windowed_pending) {
        w->windowed_pending = false;
        ApplyWindowMode(w, WindowMode::kWindowed, WindowMode::kFullscreen);
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_SYSCOMMAND:
      // Alt alone would open the system menu and stop the pump.
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
      break;
    case WM_SIZE:
      if (w) w->client.store(uint64_t(LOWORD(lp)) << 32 | HIWORD(lp));
      break;
    case WM_SETCURSOR:
      if (w && LOWORD(lp) == HTCLIENT && w->focus.load() && w->mode.load() != WindowMode::kWindowed) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_SYSCHAR:
      // Enter character of Alt-Enter (from TranslateMessage): DefWindowProc would beep.
      if (wp == VK_RETURN) return 0;
      break;
    case WM_DPICHANGED:
      // In fullscreen the window stays on the monitor; windowed: resizable accepts the
      // suggested rectangle, otherwise the client area stays the same in physical pixels.
      if (w && w->mode.load() == WindowMode::kWindowed) {
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (w->windowed_style & WS_THICKFRAME) {
          SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
          RECT c{};
          GetClientRect(hwnd, &c);
          const POINT pos{suggested->left, suggested->top};
          SetWindowedClientSize(hwnd, w->windowed_style, c.right, c.bottom, HIWORD(wp), &pos);
        }
      }
      return 0;
    case WM_SYSKEYDOWN:
      // Alt-Enter (bit 29: Alt pressed; bit 30: repeat, ignored). Enter
      // does not reach the game as a pressed key.
      if (w && alt_enter) {
        if (lp & (1 << 30)) return 0;
        SwitchMode(w, w->mode.load() == WindowMode::kWindowed ? w->fullscreen_mode : WindowMode::kWindowed);
        return 0;
      }
      [[fallthrough]];
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (w && wp < 256) w->keys[wp].store(msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, std::memory_order_relaxed);
      break;  // DefWindowProc: Alt-F4 stays the close shortcut
    case WM_SETFOCUS:
      if (w) w->focus.store(true);
      break;
    case WM_KILLFOCUS:
      // Without focus the WM_KEYUP go to other windows: keys released.
      if (w) {
        w->focus.store(false);
        w->ReleaseAllKeys();
      }
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterGameWindowClass() {
  static const bool registered = [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &GameWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

}  // namespace

Window* CreateGameWindow(std::string_view title, const WindowOptions& options, bool visible,
                         std::function<void()> on_close) {
  if (!RegisterGameWindowClass() || options.width == 0 || options.height == 0) return nullptr;
  auto* w = new Window;
  w->on_close = std::move(on_close);
  w->fullscreen_mode = options.mode == WindowMode::kWindowed ? WindowMode::kBorderless : options.mode;
  w->windowed_style = kWindowedBaseStyle |
                      (options.resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
  std::promise<HWND> created;
  auto created_future = created.get_future();
  // The promise belongs to the thread (moved into the lambda): set_value does not touch
  // a caller object, which may have already resumed after get().
  w->thread = std::thread([w, created = std::move(created), wtitle = Widen(title), options, visible]() mutable {
    SetCurrentThreadName("gow2 window");
    // Always created windowed: it is the position Alt-Enter returns to. The
    // size is set afterwards, at the DPI of the monitor it was born on.
    HWND hwnd = CreateWindowExW(0, kWindowClass, wtitle.c_str(), w->windowed_style, CW_USEDEFAULT, CW_USEDEFAULT,
                                LONG(options.width), LONG(options.height), nullptr, nullptr,
                                GetModuleHandleW(nullptr), w);
    w->hwnd = hwnd;
    if (hwnd) {
      SetWindowedClientSize(hwnd, w->windowed_style, LONG(options.width), LONG(options.height),
                            GetDpiForWindow(hwnd), nullptr);
      // An invisible window does not receive WM_SIZE on creation.
      RECT c{};
      GetClientRect(hwnd, &c);
      w->client.store(uint64_t(c.right) << 32 | uint32_t(c.bottom));
      ApplyWindowMode(w, options.mode, WindowMode::kWindowed);
    }
    created.set_value(hwnd);
    if (!hwnd) return;
    if (visible) {
      ShowWindow(hwnd, SW_SHOWNORMAL);
      UpdateWindow(hwnd);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  });
  if (!created_future.get()) {
    w->thread.join();
    delete w;
    return nullptr;
  }
  return w;
}

void* NativeHandle(Window* w) { return w ? w->hwnd : nullptr; }

std::pair<uint32_t, uint32_t> ClientSize(Window* w) {
  if (!w) return {0, 0};
  const uint64_t c = w->client.load();
  return {uint32_t(c >> 32), uint32_t(c)};
}

WindowMode CurrentWindowMode(Window* w) { return w ? w->mode.load() : WindowMode::kWindowed; }

bool ExclusiveFullscreenWanted(Window* w, bool active) {
  if (!w) return false;
  if (w->exclusive.exchange(active) && !active) PostMessageW(w->hwnd, kMsgExclusiveLeft, 0, 0);
  return w->mode.load() == WindowMode::kFullscreen && w->focus.load();
}

void RequestClose(Window* w) {
  if (w) PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
}

bool IsKeyDown(Window* w, uint8_t virtual_key) {
  return w && w->focus.load(std::memory_order_relaxed) && w->keys[virtual_key].load(std::memory_order_relaxed);
}

bool HasFocus(Window* w) { return w && w->focus.load(std::memory_order_relaxed); }

void ReleaseAllKeys(Window* w) {
  if (w) w->ReleaseAllKeys();
}

void ApplyWindowOptions(Window* w, const WindowOptions& options) {
  if (!w) return;
  {
    std::lock_guard g(w->mutex);
    w->pending_options = options;
  }
  PostMessageW(w->hwnd, kMsgApplyOptions, 0, 0);
}

void SetMouseCapture(Window* w, bool capture) {
  if (w) PostMessageW(w->hwnd, kMsgMouseCapture, capture ? 1 : 0, 0);
}

void SetMessageHook(Window* w, MessageHook hook) {
  if (!w) return;
  std::lock_guard g(w->mutex);
  w->hook = hook ? std::make_unique<const MessageHook>(std::move(hook)) : nullptr;
}

void DestroyGameWindow(Window* w) {
  if (!w) return;
  PostMessageW(w->hwnd, kMsgDestroy, 0, 0);
  if (w->thread.joinable()) w->thread.join();
  delete w;
}

namespace {
bool g_output_visible = false;

// Points stdout/stderr (and CRT descriptors 1/2, which log.cpp's crash path writes to directly) at the console.
bool ReopenOnConsole() {
  FILE* f = nullptr;
  if (freopen_s(&f, "CONOUT$", "w", stdout) != 0 || freopen_s(&f, "CONOUT$", "w", stderr) != 0) return false;
  if (_fileno(stdout) != 1 && _dup2(_fileno(stdout), 1) != 0) return false;
  if (_fileno(stderr) != 2 && _dup2(_fileno(stderr), 2) != 0) return false;
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::cout.clear();
  std::cerr.clear();
  std::clog.clear();
  return true;
}
}  // namespace

void SetupConsole(bool want_console) {
  if (g_output_visible) return;
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE) {  // inherited pipe or file: the CRT already uses it
    g_output_visible = true;
    return;
  }
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    g_output_visible = ReopenOnConsole();
  } else if (want_console && AllocConsole()) {
    // Closing our own console window would send CTRL_CLOSE_EVENT and kill the game: no close button.
    if (const HWND console = GetConsoleWindow()) DeleteMenu(GetSystemMenu(console, FALSE), SC_CLOSE, MF_BYCOMMAND);
    g_output_visible = ReopenOnConsole();
  }
}

bool OutputVisible() { return g_output_visible; }

void ShowErrorBox(std::string_view utf8_message) {
  std::wstring w(utf8_message.size() + 1, L' ');
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_message.data(), int(utf8_message.size()), w.data(), int(w.size()));
  w.resize(size_t(n));
  MessageBoxW(nullptr, w.c_str(), L"Gears of War 2", MB_OK | MB_ICONERROR);
}

}  // namespace plat
