#include "gpu/overlay.h"

#include <windows.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <array>
#include <chrono>
#include <optional>
#include <utility>

#include "launcher/imgui_style.h"
#include "launcher/launcher.h"
#include "launcher/launcher_model.h"
#include "launcher/nvidia_dlls.h"
#include "log.h"
#include "platform/platform.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gpu {

// shader-visible SRV heap of the dx12 backends (ImGui fonts and textures), free-list allocator.
struct Overlay::Heap {
  // ImGui fonts and dynamic textures (the enlarged atlas coexists for a frame with the old one): ample margin.
  static constexpr uint32_t kSize = 64;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t increment = 0;
  std::array<bool, kSize> used{};
};

namespace {

std::string Show(const std::filesystem::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

bool IsInputMessage(UINT msg) {
  return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
         msg == WM_MOUSELEAVE;
}

}  // namespace

std::unique_ptr<Overlay> Overlay::Create(plat::Window* window, std::filesystem::path config,
                                         settings::Settings running, ApplyFn on_apply,
                                         ScreenshotFn on_screenshot, ToggleFn on_shader_capture) {
  std::unique_ptr<Overlay> o(new Overlay);
  o->window_ = window;
  o->hwnd_ = plat::NativeHandle(window);
  o->config_ = std::move(config);
  o->running_ = o->current_ = std::move(running);
  o->on_apply_ = std::move(on_apply);
  o->on_screenshot_ = std::move(on_screenshot);
  o->on_shader_capture_ = std::move(on_shader_capture);
  if (window) {
    Overlay* self = o.get();
    plat::SetMessageHook(window, [self](uint32_t msg, uintptr_t wp, intptr_t lp) -> std::optional<intptr_t> {
      // F12 also with the panel open or disabled: the screenshot shows what is on screen.
      if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_F12 && self->on_screenshot_) {
        if (msg == WM_KEYDOWN && !(lp & (1 << 30))) self->on_screenshot_();  // auto-repeat ignored
        return 0;
      }
      // F11 (only with --shader-capture): starts/stops the shader capture, like F12 also with the panel open.
      if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_F11 && self->on_shader_capture_) {
        if (msg == WM_KEYDOWN && !(lp & (1 << 30))) self->on_shader_capture_();
        return 0;
      }
      if (self->disabled_.load()) return std::nullopt;
      if (msg == WM_KEYDOWN && wp == VK_F1) {
        if (!(lp & (1 << 30))) {  // auto-repeat ignored
          const bool open = !self->open_.load();
          if (open) {
            plat::ReleaseAllKeys(self->window_);
            std::lock_guard g(self->queue_mutex_);
            self->queue_.clear();  // messages from a previous opening
            ++self->opens_;
          }
          self->open_.store(open);
          if (!open) plat::SetMouseCapture(self->window_, false);
        }
        return 0;
      }
      if (msg == WM_KEYUP && wp == VK_F1) return 0;
      // Before the open_ check: closed with Esc during a drag, the release arrives with the overlay
      // closed and must still drop the capture (otherwise clicks on the desktop would go to the game).
      if ((msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) &&
          !(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)))
        plat::SetMouseCapture(self->window_, false);
      // WM_MOUSELEAVE (highlight removed on leaving the window): TrackMouseEvent only while visible, an
      // invisible window would receive it on every movement; also with the overlay closed, for reopening.
      if (msg == WM_MOUSELEAVE) self->mouse_tracked_ = false;
      if (!self->open_.load()) return std::nullopt;
      const HWND hwnd = static_cast<HWND>(self->hwnd_);
      if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW, also in full screen
        return TRUE;
      }
      // Mouse capture on the window thread (the ImGui win32 backend's SetCapture runs on the CP thread
      // and has no effect): dragging a slider past the edge keeps moving it.
      if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
        plat::SetMouseCapture(self->window_, true);
      if (msg == WM_MOUSEMOVE && !self->mouse_tracked_ && IsWindowVisible(hwnd)) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        self->mouse_tracked_ = TrackMouseEvent(&tme) != FALSE;
      }
      const bool input = IsInputMessage(msg);
      if (!input && msg != WM_SETFOCUS && msg != WM_KILLFOCUS) return std::nullopt;
      Message m{msg, wp, lp, {}};
      GetKeyboardState(m.keys.data());
      {
        std::lock_guard g(self->queue_mutex_);
        self->queue_.push_back(m);
      }
      // Alt (WM_SYS*) stays with the window: Alt-F4 still closes; the focus is also handled by the window.
      if (!input || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR) return std::nullopt;
      return 0;
    });
  }
  return o;
}

Overlay::~Overlay() {
  plat::SetMessageHook(window_, {});
  if (!context_) return;
  ImGui::SetCurrentContext(context_);
  ImGui_ImplDX12_Shutdown();
  if (hwnd_) ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext(context_);
}

bool Overlay::Init(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& why) {
  heap_ = std::make_unique<Heap>();
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = Heap::kSize;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_->heap)))) {
    why = "descriptor heap not created";
    return false;
  }
  heap_->increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  // The cursor is decided by the hook on the window thread (SetCursor from here has no effect).
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
  if (hwnd_ && !ImGui_ImplWin32_Init(hwnd_)) {
    why = "ImGui win32 backend";
    return false;
  }
  ImGui_ImplDX12_InitInfo info;
  info.Device = device;
  info.CommandQueue = queue;
  info.NumFramesInFlight = 2;  // like the presenter's back buffers, which waits for the frame from two rounds ago
  info.RTVFormat = xe::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  info.UserData = heap_.get();
  info.SrvDescriptorHeap = heap_->heap.Get();
  info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto* h = static_cast<Heap*>(i->UserData);
    uint32_t n = 0;
    while (n < Heap::kSize && h->used[n]) ++n;
    if (n == Heap::kSize) {
      // Heap full (IM_ASSERT is empty in release): last slot shared, wrong text instead of a crash.
      static bool logged = false;
      if (!std::exchange(logged, true)) LogLine("[overlay] SRV heap full: ImGui textures shared");
      n = Heap::kSize - 1;
    }
    h->used[n] = true;
    *cpu = h->heap->GetCPUDescriptorHandleForHeapStart();
    cpu->ptr += size_t(n) * h->increment;
    *gpu = h->heap->GetGPUDescriptorHandleForHeapStart();
    gpu->ptr += uint64_t(n) * h->increment;
  };
  info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto* h = static_cast<Heap*>(i->UserData);
    const size_t n = (cpu.ptr - h->heap->GetCPUDescriptorHandleForHeapStart().ptr) / h->increment;
    if (n < Heap::kSize) h->used[n] = false;
  };
  if (!ImGui_ImplDX12_Init(&info)) {
    why = "ImGui dx12 backend";
    return false;
  }
  launcher::LoadFonts();
  state_.overlay = true;
  state_.windows_italian = launcher::WindowsUiIsItalian();
  state_.dlss_available = launcher::DlssBridgePresent(launcher::ExeDir());
  state_.unavailable = launcher::UnavailableOptions();
  state_.dlss_download_available = launcher::DlssDownloadAvailable(launcher::ExeDir());
  std::tie(state_.desktop_w, state_.desktop_h) = plat::PrimaryMonitorSize();
  state_.fg_available = launcher::FrameGenerationDllsPresent(launcher::ExeDir());
  state_.dlss_fg_available = launcher::DlssFgDllsPresent(launcher::ExeDir());
  state_.optiscaler = optiscaler::Detect(launcher::ExeDir());
  state_.optiscaler_loaded = optiscaler::Loaded(launcher::ExeDir());
  state_.dlssnr_runtime = launcher::DlssNrRuntimePresent(launcher::ExeDir());
  optiscaler::SweepOld(launcher::ExeDir());  // backup copies left by a previous replacement
  state_.hdr_display = launcher::QueryHdrDisplay(hwnd_);
  monitor_modes_ = launcher::MonitorModes();  // once: enumerating them on every opening causes a hitch
  state_.resolutions = launcher::ResolutionChoices(monitor_modes_, 0, 0);
  return true;
}

void Overlay::DrawRecordingIndicator() {
  // Top-left, over everything; small: it also ends up in the capture's shots (the overlay is drawn before the copy).
  ImDrawList* d = ImGui::GetForegroundDrawList();
  const char* text = "REC shader";
  const ImVec2 size = ImGui::CalcTextSize(text);
  d->AddRectFilled(ImVec2(8, 8), ImVec2(20 + size.x + 8, 8 + size.y + 8), IM_COL32(0, 0, 0, 160), 4.0f);
  d->AddCircleFilled(ImVec2(16, 12 + size.y / 2), 4.0f, IM_COL32(230, 30, 30, 255));
  d->AddText(ImVec2(24, 12), IM_COL32(255, 255, 255, 255), text);
}

void Overlay::Draw(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list,
                   D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
  const bool open = open_.load(), recording = recording_.load();
  if (disabled_.load() || (!open && !recording)) return;
  if (!context_) {
    std::string why;
    if (!Init(device, queue, why)) {
      LogLine("[overlay] unavailable: " + why);
      disabled_.store(true);
      open_.store(false);
      return;
    }
  }
  ImGui::SetCurrentContext(context_);
  if (!open) {
    // Panel closed: only the indicator. No Win32 NewFrame (it would set the game's cursor) and no input.
    ImGui_ImplDX12_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(w), float(h));
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    DrawRecordingIndicator();
    ImGui::Render();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {heap_->heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
    return;
  }
  if (install_.valid() && install_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto err = install_.get();
    state_.optiscaler_installing = false;
    state_.optiscaler = optiscaler::Detect(launcher::ExeDir());
    if (err) {
      LogLine("[optiscaler] " + *err);
      state_.optiscaler_message = *err;
    } else {
      // The loaded dxgi.dll stays the previous one: OptiScaler takes effect from the next launch (no restart in game).
      LogLine("[optiscaler] installed: takes effect when the game restarts");
      state_.optiscaler_restart = true;
    }
  }
  if (dlss_download_.valid() && dlss_download_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto err = dlss_download_.get();
    state_.dlss_downloading = false;
    state_.dlss_download_available = launcher::DlssDownloadAvailable(launcher::ExeDir());
    state_.dlss_available = launcher::DlssBridgePresent(launcher::ExeDir());
    state_.dlss_fg_available = launcher::DlssFgDllsPresent(launcher::ExeDir());
    if (err) {
      LogLine("[nvidia_dlls] " + *err);
      state_.dlss_download_message = *err;
    } else {
      LogLine("[nvidia_dlls] downloaded: DLSS is selectable from the next launch");
      state_.dlss_download_message =
          launcher::Text(launcher::UiLanguageFor(state_.s.language, state_.windows_italian)).dlss_download_done;
    }
  }
  if (opens_seen_ != opens_.load()) {  // new opening: restart from the settings in force
    opens_seen_ = opens_.load();
    // Keys and mouse of the last opening: releases queued after closing were lost.
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    state_.s = current_;
    state_.error.clear();
    state_.save_failed = false;
    state_.notice = launcher::Notice::kNone;
    state_.focus_play = true;
    state_.resolutions =
        launcher::ResolutionChoices(monitor_modes_, current_.video.width, current_.video.height);
  }
  const UINT dpi = hwnd_ ? GetDpiForWindow(static_cast<HWND>(hwnd_)) : 96;
  // Scale from the value being edited: the dropdown takes effect immediately (ApplyScale only on change, like the window
  // resize; dynamic fonts, no atlas to rebuild). Without Apply the reopening restarts from current_.
  const uint32_t ui_scale = state_.s.video.ui_scale;
  const float scale = launcher::PanelScale(float(dpi ? dpi : 96) / 96, int(h), 0, ui_scale);
  if (scale != scale_) {
    launcher::ApplyScale(scale);
    scale_ = scale;
  }
  state_.panel_w = launcher::kWindowWidth * scale;
  state_.panel_h = launcher::PanelHeight(scale, ui_scale, int(h));  // above 100% the panel is capped to the window: it scrolls

  std::vector<Message> messages;
  {
    std::lock_guard g(queue_mutex_);
    messages.swap(queue_);
  }
  if (hwnd_)
    for (const Message& m : messages) {
      SetKeyboardState(const_cast<BYTE*>(m.keys.data()));  // Ctrl/Shift/Alt as on the window thread
      ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd_), m.msg, WPARAM(m.wp), LPARAM(m.lp));
    }
  // Mouse capture and TrackMouseEvent are done by the hook on the window thread (from here they have no effect).

  ImGui_ImplDX12_NewFrame();
  if (hwnd_) ImGui_ImplWin32_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(w), float(h));
  if (!hwnd_) io.DeltaTime = 1.0f / 60;
  // Before NewFrame: NewFrame itself closes the combo (or releases the slider) with Esc.
  const bool busy = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetActiveID() != 0;
  ImGui::NewFrame();
  const bool escape = !busy && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  const launcher::Action action = escape ? launcher::Action::kQuit : launcher::DrawSettings(state_);
  if (recording) DrawRecordingIndicator();
  ImGui::Render();
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  ID3D12DescriptorHeap* heaps[] = {heap_->heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);

  switch (action) {
    case launcher::Action::kPlay:
      Apply();
      break;
    case launcher::Action::kQuit:
      Close();
      break;
    case launcher::Action::kDefaults: {
      auto cvars = std::move(state_.s.gpu_cvars);  // the cvars have no controls: they stay
      state_.s = {};
      state_.s.gpu_cvars = std::move(cvars);
      break;
    }
    case launcher::Action::kInstallOptiScaler:
      if (!install_.valid()) {
        state_.optiscaler_installing = true;
        state_.optiscaler_message.clear();
        install_ = std::async(std::launch::async, [dir = launcher::ExeDir()] { return optiscaler::Install(dir); });
      }
      break;
    case launcher::Action::kOpenOptiScaler:
      Close();  // like Close, without applying; then INSERT to the game window
      optiscaler::OpenMenu(hwnd_);
      break;
    case launcher::Action::kDownloadDlss:
      if (!dlss_download_.valid()) {
        state_.dlss_downloading = true;
        state_.dlss_download_message.clear();
        dlss_download_ =
            std::async(std::launch::async, [dir = launcher::ExeDir()] { return nvidia_dlls::Install(dir); });
      }
      break;
    case launcher::Action::kChooseIso:     // launcher only
    case launcher::Action::kCancelExtract:
    case launcher::Action::kNone:
      break;
  }
}

void Overlay::Apply() {
  const settings::Settings& s = state_.s;
  if (const auto err = settings::SaveFile(config_, s)) {
    state_.error = *err;
    state_.save_failed = true;
    state_.notice = launcher::Notice::kNone;
    LogLine("[overlay] save failed: " + *err);
    return;
  }
    LogLine("[overlay] settings saved to " + Show(config_));
  current_ = s;
  state_.error.clear();
  state_.save_failed = false;
  state_.restart = launcher::RestartNeeded(running_, s);
  state_.notice = state_.restart.empty() ? launcher::Notice::kNone : launcher::Notice::kRestart;
  if (on_apply_) on_apply_(s);
}

bool Overlay::PopupOpen() const {
  if (!context_) return false;
  ImGui::SetCurrentContext(context_);
  return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
}

void Overlay::Close() {
  open_.store(false);
  std::lock_guard g(queue_mutex_);
  queue_.clear();
}

}  // namespace gpu
