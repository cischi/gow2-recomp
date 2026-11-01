// In-game F1 overlay: the launcher's settings panel
// (launcher::DrawSettings) over the game, drawn by the presenter with Dear ImGui (dx12).
//
//  - Threads: the message hook (plat::SetMessageHook) runs on the window thread and does not touch
//    ImGui: F1 opens/closes (atomic), with the panel open keyboard, characters and mouse are queued (mutex)
//    and consumed; the cursor becomes the arrow. Draw runs in Present (command processor thread),
//    replays the queue with ImGui_ImplWin32_WndProcHandler and draws.
//  - Apply: SaveFile(config, s), then on_apply(s) (on the Draw thread); notes
//    "Valgono dal prossimo avvio: ..." for the RestartNeeded fields compared with `running`.
//  - Menu in English or Italian like the launcher (launcher/launcher_text.h).
//  - Scale: launcher::PanelScale with the panel's [video] ui_scale, immediately (not in RestartNeeded).
//  - Close, Esc (with no open combos) or F1: closes without applying; it reopens from the last applied
//    settings (initially `running`).
//  - ImGui context and backend created at the first Draw with the panel open; if they fail
//    `[overlay] unavailable: <reason>` and F1 does nothing any more.
#pragma once

#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "launcher/launcher_ui.h"
#include "settings.h"

struct ImGuiContext;
namespace plat {
struct Window;
}

namespace gpu {

class Overlay {
 public:
  using ApplyFn = std::function<void(const settings::Settings&)>;
  using ScreenshotFn = std::function<void()>;
  using ToggleFn = std::function<void()>;
  // null window: no hook (the panel never opens). on_screenshot: called on the window thread at every F12
  // press (empty: F12 goes to the game). on_shader_capture: the same for F11 (--shader-capture only).
  static std::unique_ptr<Overlay> Create(plat::Window* window, std::filesystem::path config,
                                         settings::Settings running, ApplyFn on_apply,
                                         ScreenshotFn on_screenshot = {}, ToggleFn on_shader_capture = {});
  // Removes the hook, then destroys backend and context (GPU already stopped: the presenter guarantees it).
  ~Overlay();

  bool IsOpen() const { return open_.load(); }
  // Shader capture running: "REC shader" in the top-left corner, also with the panel closed. Any thread.
  void SetRecording(bool on) { recording_.store(on); }
  // Draw has something to draw: the panel or the recording indicator.
  bool Visible() const { return open_.load() || recording_.load(); }
  // Panel state and open combo (tests; Draw thread).
  const launcher::LauncherState& State() const { return state_; }
  bool PopupOpen() const;
  // Inside Present: back buffer w x h in RENDER_TARGET behind rtv, format kGuestOutputFormat.
  // With the panel closed it draws only the recording indicator (SetRecording), or nothing.
  void Draw(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list,
            D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);

 private:
  struct Message {
    uint32_t msg;
    uintptr_t wp;
    intptr_t lp;
    // GetKeyboardState of the window thread: the win32 backend reads the modifiers with
    // GetKeyState, which applies to the calling thread (SetKeyboardState before the replay).
    std::array<uint8_t, 256> keys;
  };
  Overlay() = default;
  bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& why);
  void Apply();
  void Close();
  void DrawRecordingIndicator();

  plat::Window* window_ = nullptr;
  void* hwnd_ = nullptr;
  bool mouse_tracked_ = false;  // TrackMouseEvent active (window thread only, in the hook)
  std::filesystem::path config_;
  settings::Settings running_, current_;
  std::vector<launcher::Resolution> monitor_modes_;  // modes of the primary monitor, read in Init
  ApplyFn on_apply_;
  ScreenshotFn on_screenshot_;
  ToggleFn on_shader_capture_;

  std::atomic<bool> open_{false};
  std::atomic<bool> recording_{false};
  std::atomic<uint32_t> opens_{0};  // openings with F1 (the hook empties the queue on every opening)
  std::atomic<bool> disabled_{false};
  std::mutex queue_mutex_;
  std::vector<Message> queue_;

  // Draw thread.
  ImGuiContext* context_ = nullptr;
  uint32_t opens_seen_ = 0;  // opening already prepared by Draw
  float scale_ = 0;
  launcher::LauncherState state_;
  // OptiScaler installation on another thread; Draw reads its outcome. The game exits with _Exit (~Overlay does not
  // run): the exits of main.cpp and the kernel first call CancelInstall + WaitInstallCopy (download cancelled,
  // copy already begun carried to completion). If ~Overlay runs, the future's destructor waits for the thread.
  std::future<std::optional<std::string>> install_;
  std::future<std::optional<std::string>> dlss_download_;  // nvidia_dlls::Install
  struct Heap;
  std::unique_ptr<Heap> heap_;
};

}  // namespace gpu
