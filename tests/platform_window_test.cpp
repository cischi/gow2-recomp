// Game window (platform/platform.h): dedicated thread with a message pump,
// WM_CLOSE -> on_close only once.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "platform/platform.h"

using namespace std::chrono_literals;

TEST_CASE("platform_window: invisible window, RequestClose calls on_close once within 1 s",
          "[platform_window]") {
  std::atomic<int> closes{0};
  std::atomic<std::thread::id> close_thread{};
  plat::Window* w = plat::CreateGameWindow("gow2 test", {1280, 720}, false, [&] {
    close_thread = std::this_thread::get_id();
    closes.fetch_add(1);
  });
  REQUIRE(w != nullptr);
  CHECK(plat::NativeHandle(w) != nullptr);

  plat::RequestClose(w);
  const auto end = std::chrono::steady_clock::now() + 1s;
  while (closes.load() == 0 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
  CHECK(closes.load() == 1);
  CHECK(close_thread.load() != std::this_thread::get_id());  // on the window thread

  // a second request does not call on_close again
  plat::RequestClose(w);
  std::this_thread::sleep_for(100ms);
  CHECK(closes.load() == 1);

  plat::DestroyGameWindow(w);
}

TEST_CASE("platform_window: client area of the requested size, destruction without closing",
          "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test", {640, 360}, false, [] {});
  REQUIRE(w != nullptr);
  const auto size = plat::ClientSize(w);
  CHECK(size.first == 640);
  CHECK(size.second == 360);
  plat::DestroyGameWindow(w);
  plat::DestroyGameWindow(nullptr);  // allowed
}

// Monitor refresh rate and VRR state: without NVAPI (non-NVIDIA GPU) it means "unavailable", never a crash.
TEST_CASE("platform_window: QueryDisplaySync gives the refresh rate and, with NVAPI, VRR or the reason", "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test", {640, 360}, false, [] {});
  REQUIRE(w != nullptr);
  const plat::DisplaySync off = plat::QueryDisplaySync(w, false);
  CHECK(off.refresh_hz >= 24);
  CHECK_FALSE(off.vrr.has_value());
  CHECK(off.nvapi_us == 0);
  CHECK(off.note.empty());
  const plat::DisplaySync on = plat::QueryDisplaySync(w, true);
  CHECK(on.refresh_hz >= 24);
  CHECK((on.vrr.has_value() || !on.note.empty()));
  plat::DestroyGameWindow(w);
}

// Key state from the window queue (WM_KEYDOWN/WM_KEYUP) and focus
// (WM_SETFOCUS/WM_KILLFOCUS). Messages are posted directly: the window
// must have focus though, which is requested from the OS (SKIP without an
// interactive desktop or if Windows denies the foreground).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

template <typename F>
bool WaitUntil(F cond, std::chrono::milliseconds timeout) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (!cond()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

}  // namespace

TEST_CASE("platform_window: IsKeyDown and HasFocus from the window messages", "[platform_window]") {
  CHECK_FALSE(plat::IsKeyDown(nullptr, VK_RETURN));
  CHECK_FALSE(plat::HasFocus(nullptr));

  plat::Window* w = plat::CreateGameWindow("gow2 test keyboard", {320, 200}, true, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  // Foreground: Windows grants it only if the test process can take it
  // (started by the foreground process); otherwise SKIP.
  SetForegroundWindow(hwnd);
  if (!WaitUntil([&] { return plat::HasFocus(w); }, 1s)) {
    plat::DestroyGameWindow(w);
    SKIP("the window does not receive focus (session without an interactive desktop)");
  }

  CHECK_FALSE(plat::IsKeyDown(w, VK_RETURN));
  PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
  CHECK(WaitUntil([&] { return plat::IsKeyDown(w, VK_RETURN); }, 1s));
  PostMessageW(hwnd, WM_KEYDOWN, 'W', 0x00110001);
  CHECK(WaitUntil([&] { return plat::IsKeyDown(w, 'W'); }, 1s));
  CHECK(plat::IsKeyDown(w, VK_RETURN));

  PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
  CHECK(WaitUntil([&] { return !plat::IsKeyDown(w, VK_RETURN); }, 1s));
  CHECK(plat::IsKeyDown(w, 'W'));

  PostMessageW(hwnd, WM_KEYDOWN, VK_SPACE, 0x00390001);
  CHECK(WaitUntil([&] { return plat::IsKeyDown(w, VK_SPACE); }, 1s));
  // focus lost: all keys released
  PostMessageW(hwnd, WM_KILLFOCUS, 0, 0);
  CHECK(WaitUntil([&] { return !plat::HasFocus(w); }, 1s));
  CHECK_FALSE(plat::IsKeyDown(w, VK_SPACE));
  CHECK_FALSE(plat::IsKeyDown(w, 'W'));
  CHECK_FALSE(plat::IsKeyDown(w, VK_RETURN));

  plat::DestroyGameWindow(w);
}

// Window modes (styles, WM_SIZE, Alt-Enter). The windows are
// invisible: SetWindowPos and SendMessage reach the window thread synchronously.
TEST_CASE("platform_window: resizable windowed follows SetWindowPos", "[platform_window]") {
  plat::WindowOptions opt;
  opt.resizable = true;
  plat::Window* w = plat::CreateGameWindow("gow2 test", opt, false, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
  CHECK((style & WS_THICKFRAME) != 0);
  CHECK(plat::CurrentWindowMode(w) == plat::WindowMode::kWindowed);
  RECT r{0, 0, 960, 540};  // smaller than the default: no screen limit
  AdjustWindowRectEx(&r, style, FALSE, 0);
  SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
  CHECK(plat::ClientSize(w) == std::pair<uint32_t, uint32_t>{960, 540});
  plat::DestroyGameWindow(w);
}

TEST_CASE("platform_window: non-resizable windowed has no draggable border", "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test", {1280, 720}, false, [] {});
  REQUIRE(w != nullptr);
  const DWORD style = DWORD(GetWindowLongPtrW(static_cast<HWND>(plat::NativeHandle(w)), GWL_STYLE));
  CHECK((style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == 0);
  CHECK((style & WS_CAPTION) == WS_CAPTION);
  plat::DestroyGameWindow(w);
}

namespace {
std::pair<uint32_t, uint32_t> MonitorSize(HWND hwnd) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  return {uint32_t(mi.rcMonitor.right - mi.rcMonitor.left), uint32_t(mi.rcMonitor.bottom - mi.rcMonitor.top)};
}
}  // namespace

TEST_CASE("platform_window: borderless covers the monitor with WS_POPUP", "[platform_window]") {
  plat::WindowOptions opt;
  opt.mode = plat::WindowMode::kBorderless;
  plat::Window* w = plat::CreateGameWindow("gow2 test", opt, false, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
  CHECK((style & WS_POPUP) != 0);
  CHECK((style & (WS_CAPTION | WS_THICKFRAME)) == 0);
  CHECK(plat::CurrentWindowMode(w) == plat::WindowMode::kBorderless);
  CHECK(plat::ClientSize(w) == MonitorSize(hwnd));
  plat::DestroyGameWindow(w);
}

TEST_CASE("platform_window: Alt-Enter toggles between windowed and borderless", "[platform_window]") {
  plat::WindowOptions opt;
  opt.width = 800;
  opt.height = 600;
  opt.resizable = true;
  plat::Window* w = plat::CreateGameWindow("gow2 test", opt, false, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  constexpr LPARAM kAltEnter = 0x201C0001;  // bit 29: Alt pressed
  const DWORD windowed_style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
  RECT before{};
  GetWindowRect(hwnd, &before);

  SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, kAltEnter);
  CHECK(plat::CurrentWindowMode(w) == plat::WindowMode::kBorderless);
  CHECK((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) != 0);
  CHECK(plat::ClientSize(w) == MonitorSize(hwnd));
  CHECK_FALSE(plat::IsKeyDown(w, VK_RETURN));

  // auto-repeat (bit 30): no change
  SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, kAltEnter | 0x40000000);
  CHECK(plat::CurrentWindowMode(w) == plat::WindowMode::kBorderless);

  SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, kAltEnter);
  CHECK(plat::CurrentWindowMode(w) == plat::WindowMode::kWindowed);
  CHECK(DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)) == windowed_style);
  CHECK(plat::ClientSize(w) == std::pair<uint32_t, uint32_t>{800, 600});
  RECT after{};
  GetWindowRect(hwnd, &after);
  CHECK(after.left == before.left);
  CHECK(after.top == before.top);
  CHECK(after.right == before.right);
  CHECK(after.bottom == before.bottom);
  CHECK_FALSE(IsWindowVisible(hwnd));
  plat::DestroyGameWindow(w);
}

TEST_CASE("platform_window: the hook consumes messages and consumed keys appear released",
          "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test hook", {320, 200}, true, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  SetForegroundWindow(hwnd);
  if (!WaitUntil([&] { return plat::HasFocus(w); }, 1s)) {
    plat::DestroyGameWindow(w);
    SKIP("the window does not receive focus (session without an interactive desktop)");
  }
  SendMessageW(hwnd, WM_KEYDOWN, 'W', 0x00110001);
  CHECK(plat::IsKeyDown(w, 'W'));

  std::atomic<int> keydowns{0};
  plat::SetMessageHook(w, [&](uint32_t msg, uintptr_t wp, intptr_t) -> std::optional<intptr_t> {
    if (msg != WM_KEYDOWN) return std::nullopt;
    ++keydowns;
    if (wp == VK_F1 || wp == 'W') return 0;
    return std::nullopt;
  });
  SendMessageW(hwnd, WM_KEYDOWN, 'W', 0x40110001);  // repeat: consumed, the key appears released
  CHECK_FALSE(plat::IsKeyDown(w, 'W'));
  SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0x003B0001);
  CHECK_FALSE(plat::IsKeyDown(w, VK_F1));
  SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
  CHECK(plat::IsKeyDown(w, VK_RETURN));
  CHECK(keydowns.load() == 3);
  plat::ReleaseAllKeys(w);
  CHECK_FALSE(plat::IsKeyDown(w, VK_RETURN));

  plat::SetMessageHook(w, {});
  SendMessageW(hwnd, WM_KEYDOWN, 'W', 0x00110001);
  CHECK(plat::IsKeyDown(w, 'W'));
  CHECK(keydowns.load() == 3);
  plat::DestroyGameWindow(w);
}

TEST_CASE("platform_window: ApplyWindowOptions in windowed mode changes the client area and resizability", "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test", {320, 200}, false, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  CHECK((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) == 0);
  plat::ApplyWindowOptions(w, {640, 360, plat::WindowMode::kWindowed, true});
  CHECK(WaitUntil([&] { return plat::ClientSize(w) == std::pair<uint32_t, uint32_t>{640, 360}; }, 1s));
  CHECK((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0);
  CHECK(plat::CurrentWindowMode(w) == plat::WindowMode::kWindowed);
  plat::DestroyGameWindow(w);
}

TEST_CASE("platform_window: ApplyWindowOptions switches to borderless and back to windowed", "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test", {320, 200}, false, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  plat::ApplyWindowOptions(w, {1280, 720, plat::WindowMode::kBorderless, false});
  CHECK(WaitUntil([&] { return plat::CurrentWindowMode(w) == plat::WindowMode::kBorderless; }, 1s));
  CHECK((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) != 0);
  CHECK(WaitUntil([&] { return plat::ClientSize(w) == MonitorSize(hwnd); }, 1s));
  plat::ApplyWindowOptions(w, {400, 300, plat::WindowMode::kWindowed, false});
  CHECK(WaitUntil([&] { return plat::CurrentWindowMode(w) == plat::WindowMode::kWindowed; }, 1s));
  CHECK(WaitUntil([&] { return plat::ClientSize(w) == std::pair<uint32_t, uint32_t>{400, 300}; }, 1s));
  CHECK((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) == 0);
  plat::DestroyGameWindow(w);
}

// F1 overlay: mouse capture is taken and released on the window thread (SetCapture only works there).
TEST_CASE("platform_window: SetMouseCapture on the window thread", "[platform_window]") {
  plat::Window* w = plat::CreateGameWindow("gow2 test", {640, 360}, false, [] {});
  REQUIRE(w != nullptr);
  const HWND hwnd = static_cast<HWND>(plat::NativeHandle(w));
  const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
  auto captured = [&] {
    GUITHREADINFO gui{sizeof(gui)};
    return GetGUIThreadInfo(tid, &gui) && gui.hwndCapture == hwnd;
  };
  auto wait_for = [&](bool want) {
    const auto end = std::chrono::steady_clock::now() + 1s;
    while (captured() != want && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    return captured() == want;
  };
  plat::SetMouseCapture(w, true);
  CHECK(wait_for(true));
  plat::SetMouseCapture(w, false);
  CHECK(wait_for(false));
  plat::SetMouseCapture(nullptr, true);  // nothing
  plat::DestroyGameWindow(w);
}
