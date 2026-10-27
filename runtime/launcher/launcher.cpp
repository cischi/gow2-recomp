#include "launcher/launcher.h"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <optional>
#include <sstream>
#include <vector>

#include "game_data.h"
#include "launcher/game_setup.h"
#include "launcher/imgui_style.h"
#include "launcher/launcher_model.h"
#include "launcher/nvidia_dlls.h"
#include "launcher/optiscaler.h"
#include "log.h"
#include "run_extract.h"
#include "settings.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace launcher {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClass[] = L"gow2_launcher";

// State seen by the window procedure (one launcher at a time).
bool g_close = false;
UINT g_dpi_changed = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
  switch (msg) {
    case WM_CLOSE:
      g_close = true;
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      g_dpi_changed = HIWORD(wp);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt alone does not open the system menu
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}


// Client area kWindowWidth x kWindowHeight at the PanelScale scale (DPI, reduced by FitScale to fit the work
// area, times ui_scale %); above 100% at most as tall as the work area (PanelHeight: the panel scrolls).
// Centered in the work area of the window's monitor. Returns the scale.
float PlaceWindow(HWND hwnd, DWORD style, UINT dpi, uint32_t ui_scale) {
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
  const int work_h = mi.rcWork.bottom - mi.rcWork.top, frame_h = frame.bottom - frame.top;
  const float scale = PanelScale(float(dpi) / 96, work_h, frame_h, ui_scale);
  RECT r{0, 0, int(kWindowWidth * scale), int(PanelHeight(scale, ui_scale, work_h - frame_h))};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
  const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
  SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  return scale;
}

struct D3D {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swap_chain;
  ComPtr<ID3D11RenderTargetView> rtv;
};

bool CreateD3D(HWND hwnd, D3D& d, std::string& why) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  HRESULT hr = E_FAIL;
  for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                       &d.swap_chain, &d.device, nullptr, &d.context);
    if (SUCCEEDED(hr)) break;
  }
  if (FAILED(hr)) {
    char b[64];
    std::snprintf(b, sizeof b, "Direct3D 11 unavailable (0x%08lX)", static_cast<unsigned long>(hr));
    why = b;
    return false;
  }
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(d.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
      FAILED(d.device->CreateRenderTargetView(back.Get(), nullptr, &d.rtv))) {
    why = "launcher back buffer unavailable";
    return false;
  }
  return true;
}

// Yes/No box with a UTF-8 text from launcher_text.
bool ConfirmOverwrite(HWND owner, const char* text) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  std::wstring w(n > 0 ? n : 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text, -1, w.data(), n);
  return MessageBoxW(owner, w.c_str(), L"Gears of War 2", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

// Total size of the files under dir (0 if it does not exist); unreadable entries are skipped.
uint64_t DirBytes(const std::filesystem::path& dir) {
  std::error_code ec;
  uint64_t total = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != decltype(it){}; it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec)) continue;
    const uint64_t size = it->file_size(fec);
    if (!fec) total += size;
  }
  return total;
}

// Open-file dialog for the ISO; empty path if cancelled.
std::filesystem::path PickIso(HWND owner, const wchar_t* title) {
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn{sizeof ofn};
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"ISO (*.iso)\0*.iso\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? std::filesystem::path(file) : std::filesystem::path();
}

bool DataReady(const std::filesystem::path& dir) {
  return game_data::ExtractionComplete(dir) && game_data::CheckXex(dir) == game_data::XexCheck::kOk;
}

}  // namespace

bool WindowsUiIsItalian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN; }

std::filesystem::path ExeDir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path();
}

namespace {
bool FilePresent(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}
}  // namespace

bool DlssBridgePresent(const std::filesystem::path& dir) {
  return FilePresent(dir / "gow2_dlss.dll") && FilePresent(dir / "nvngx_dlss.dll");
}

bool DlssDownloadAvailable(const std::filesystem::path& dir) {
  return FilePresent(dir / "gow2_dlss.dll") &&
         !(FilePresent(dir / "nvngx_dlss.dll") && FilePresent(dir / "nvngx_dlssg.dll"));
}

bool DlssNrRuntimePresent(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / "nvngx_dlssnr.dll", ec);
}

bool FrameGenerationDllsPresent(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::is_regular_file(dir / "amd_fidelityfx_loader_dx12.dll", ec) &&
         std::filesystem::is_regular_file(dir / "amd_fidelityfx_framegeneration_dx12.dll", ec);
}

bool DlssFgDllsPresent(const std::filesystem::path& dir) {
  std::error_code ec;
  return FilePresent(dir / "gow2_dlss.dll") && std::filesystem::is_regular_file(dir / "nvngx_dlssg.dll", ec);
}

hdr::Display QueryHdrDisplay(void* hwnd) {
  using Microsoft::WRL::ComPtr;
  RECT wr{};
  ComPtr<IDXGIFactory1> factory;
  if (!hwnd || !GetWindowRect(static_cast<HWND>(hwnd), &wr) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return {};
  std::vector<DXGI_OUTPUT_DESC1> descs;
  std::vector<hdr::Rect> rects;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    ComPtr<IDXGIOutput> out;
    for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
      ComPtr<IDXGIOutput6> out6;
      DXGI_OUTPUT_DESC1 d{};
      if (FAILED(out.As(&out6)) || FAILED(out6->GetDesc1(&d))) continue;
      const RECT& r = d.DesktopCoordinates;
      descs.push_back(d);
      rects.push_back({r.left, r.top, r.right, r.bottom});
    }
  }
  if (descs.empty()) return {};
  const int i = hdr::ChooseOutput({wr.left, wr.top, wr.right, wr.bottom}, rects);
  const DXGI_OUTPUT_DESC1& d = descs[i < 0 ? 0 : i];
  hdr::Display display;
  display.hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  display.max_nits = d.MaxLuminance;
  char name[3 * 32 + 1] = {};
  WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, name, sizeof name, nullptr, nullptr);
  display.name = name;
  if (!display.hdr) return display;
  // Windows SDR white of the active path with the same GDI source.
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return display;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
    return display;
  for (UINT32 p = 0; p < np; ++p) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(src), paths[p].sourceInfo.adapterId,
                  paths[p].sourceInfo.id};
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, d.DeviceName))
      continue;
    DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
    wl.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(wl), paths[p].targetInfo.adapterId,
                 paths[p].targetInfo.id};
    if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS) display.sdr_white_nits = wl.SDRWhiteLevel / 1000.0f * 80.0f;
    break;
  }
  return display;
}

std::vector<Resolution> MonitorModes() {
  std::vector<Resolution> modes;
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) modes.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
  return modes;
}

Result Run(const std::filesystem::path& config, const LaunchOptions& options, std::string* error) {
  std::string why;
  const auto fail = [&](std::string w) {
    if (error) *error = std::move(w);
    return Result::kError;
  };

  LauncherState st;
  std::error_code ec;
  if (const auto err = settings::LoadFile(config, st.s)) {
    st.error = *err;
    st.notice = Notice::kInvalidFile;
  } else if (std::filesystem::exists(config, ec)) {
    st.notice = Notice::kCommentsLost;
  }
  st.resolutions = ResolutionChoices(MonitorModes(), st.s.video.width, st.s.video.height);
  st.windows_italian = WindowsUiIsItalian();
  st.dlss_available = DlssBridgePresent(ExeDir());
  st.dlss_download_available = DlssDownloadAvailable(ExeDir());
  std::tie(st.desktop_w, st.desktop_h) = plat::PrimaryMonitorSize();
  st.fg_available = FrameGenerationDllsPresent(ExeDir());
  st.dlss_fg_available = DlssFgDllsPresent(ExeDir());
  st.optiscaler = optiscaler::Detect(ExeDir());
  st.dlssnr_runtime = DlssNrRuntimePresent(ExeDir());
  optiscaler::SweepOld(ExeDir());  // backup copies left by a previous replacement
  // OptiScaler installation (download and unpack: seconds) outside the window thread. The destructor of the
  // std::async future waits for the thread: closing during the installation cancels it (CancelInstall, after the
  // loop), so the wait is short (a copy that has already started finishes).
  std::future<std::optional<std::string>> install;
  const std::filesystem::path data_dir = game_data::ResolveDataDir(ExeDir());
  st.data_ready = DataReady(data_dir);
  Extraction extraction;
  std::future<std::optional<std::string>> dlss_download;  // nvidia_dlls::Install; cancelled on close like the OptiScaler install
  const bool italian = UiLanguageFor(st.s.language, st.windows_italian) == UiLanguage::kItalian;

  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))  /* IDC_ARROW */;
  wc.lpszClassName = kClass;
  if (!RegisterClassExW(&wc)) return fail("window class not registered");
  constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  HWND hwnd = CreateWindowExW(0, kClass, italian ? L"Gears of War 2 - Impostazioni" : L"Gears of War 2 - Settings", kStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                              kWindowWidth, kWindowHeight, nullptr, nullptr, inst, nullptr);
  if (!hwnd) {
    UnregisterClassW(kClass, inst);
    return fail("window not created");
  }
  UINT dpi = GetDpiForWindow(hwnd);
  uint32_t ui_scale = st.s.video.ui_scale;  // last value used for the window
  float scale = PlaceWindow(hwnd, kStyle, dpi, ui_scale);
  st.hdr_display = QueryHdrDisplay(hwnd);  // after PlaceWindow: the output is the window's

  Result result = Result::kQuit;
  {
    D3D d;
    if (!CreateD3D(hwnd, d, why)) {
      DestroyWindow(hwnd);
      UnregisterClassW(kClass, inst);
      return fail(why);
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    LoadFonts();
    ApplyScale(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d.device.Get(), d.context.Get());

    g_close = false;
    g_dpi_changed = 0;
    for (uint32_t frame = 1;; ++frame) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (g_close) break;  // kQuit
      // New DPI (WM_DPICHANGED) or Interface scale changed in the panel (also by Defaults): the window is
      // redone immediately, without restarting the launcher.
      if (g_dpi_changed || st.s.video.ui_scale != ui_scale) {
        if (g_dpi_changed) dpi = g_dpi_changed;
        g_dpi_changed = 0;
        ui_scale = st.s.video.ui_scale;
        ApplyScale(PlaceWindow(hwnd, kStyle, dpi, ui_scale));  // redone on the new monitor or with the new scale
        // The swap chain follows the new client area.
        d.rtv.Reset();
        d.swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        ComPtr<ID3D11Texture2D> back;
        if (SUCCEEDED(d.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back))))
          d.device->CreateRenderTargetView(back.Get(), nullptr, &d.rtv);
      }
      if (IsIconic(hwnd)) {
        Sleep(16);
        continue;
      }

      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      Action action = DrawSettings(st);
      if (options.max_frames && frame >= options.max_frames && action == Action::kNone)
        action = options.auto_action == Action::kNone ? Action::kQuit : options.auto_action;
      ImGui::Render();
      static constexpr float kClear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
      if (d.rtv) {
        d.context->OMSetRenderTargets(1, d.rtv.GetAddressOf(), nullptr);
        d.context->ClearRenderTargetView(d.rtv.Get(), kClear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      }
      d.swap_chain->Present(1, 0);

      if (action == Action::kQuit) break;
      if (action == Action::kInstallOptiScaler && !install.valid()) {
        st.optiscaler_installing = true;
        st.optiscaler_message.clear();
        install = std::async(std::launch::async, [dir = ExeDir()] { return optiscaler::Install(dir); });
      }
      if (install.valid() && install.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto err = install.get();
        st.optiscaler_installing = false;
        st.optiscaler = optiscaler::Detect(ExeDir());
        if (err) {
          LogLine("[optiscaler] " + *err);
          st.optiscaler_message = *err;
        } else {
          LogLine("[optiscaler] installed, restarting the launcher");
          // The system dxgi.dll is already loaded (the launcher's D3D11): restart with the same arguments
          // (--launcher included), without saving the settings.
          const std::filesystem::path exe = plat::ExecutablePath();
          std::wstring cmd = GetCommandLineW();  // CreateProcessW can modify the command line
          STARTUPINFOW si{sizeof si};
          PROCESS_INFORMATION pi{};
          if (!exe.empty() && CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            break;  // kQuit
          }
          LogLine("[optiscaler] restart failed (error " + std::to_string(GetLastError()) + ")");
          st.optiscaler_restart = true;
          st.optiscaler_message = Text(UiLanguageFor(st.s.language, st.windows_italian)).optiscaler_restart_manual;
        }
      }
      const UiText& tx = Text(UiLanguageFor(st.s.language, st.windows_italian));
      if (action == Action::kDownloadDlss && !dlss_download.valid()) {
        st.dlss_downloading = true;
        st.dlss_download_message.clear();
        dlss_download = std::async(std::launch::async, [dir = ExeDir()] { return nvidia_dlls::Install(dir); });
      }
      if (dlss_download.valid() && dlss_download.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto err = dlss_download.get();
        st.dlss_downloading = false;
        st.dlss_available = DlssBridgePresent(ExeDir());
        st.dlss_fg_available = DlssFgDllsPresent(ExeDir());
        st.dlss_download_available = DlssDownloadAvailable(ExeDir());
        if (err) {
          LogLine("[nvidia_dlls] " + *err);
          st.dlss_download_message = *err;
        } else {
          LogLine("[nvidia_dlls] downloaded");
          st.dlss_download_message = tx.dlss_download_done;
        }
      }
      if (action == Action::kChooseIso && !extraction.running()) {
        const auto iso = PickIso(hwnd, italian ? L"Scegli ISO" : L"Choose ISO");
        if (!iso.empty() && (!st.data_ready || ConfirmOverwrite(hwnd, tx.overwrite_data))) {
          // The files being replaced free their space: count the current folder as available.
          const uint64_t free = FreeSpaceFor(data_dir), existing = DirBytes(data_dir);
          const IsoCheck check = CheckIso(iso, free > UINT64_MAX - existing ? UINT64_MAX : free + existing);
          switch (check) {
            case IsoCheck::kOk: st.data_message.clear(); break;
            case IsoCheck::kNotAnIso: st.data_message = tx.iso_not_an_iso; break;
            case IsoCheck::kNoXex: st.data_message = tx.iso_no_xex; break;
            case IsoCheck::kWrongEdition: st.data_message = tx.iso_wrong_edition; break;
            case IsoCheck::kNoSpace: st.data_message = tx.iso_no_space; break;
          }
          if (check == IsoCheck::kOk) {
            st.data_ready = false;  // the folder is overwritten: Play stays off until the check passes again
            st.extracting = true;
            st.extract_progress = 0;
            extraction.Start(iso, data_dir);
          }
        }
      }
      if (action == Action::kCancelExtract) extraction.cancel = true;
      if (st.extracting) {
        const uint64_t total = extraction.total;
        st.extract_progress = total ? float(double(extraction.done) / double(total)) : 0.0f;
        if (extraction.Finished()) {
          const int rc = extraction.result.get();
          st.extracting = false;
          if (!extraction.log.str().empty()) LogLine("[extract] " + extraction.log.str());
          st.data_ready = DataReady(data_dir);
          if (rc == 3) st.data_message = tx.extract_cancelled;
          else if (rc != 0) st.data_message = tx.extract_failed + extraction.LastLogLine();
          else if (!st.data_ready) st.data_message = tx.extract_wrong_edition;
          else st.data_message.clear();
        }
      }
      if (action == Action::kDefaults) {
        auto cvars = std::move(st.s.gpu_cvars);  // the cvars have no controls: they stay
        st.s = {};
        st.s.gpu_cvars = std::move(cvars);
      } else if (action == Action::kPlay) {
        if (const auto err = settings::SaveFile(config, st.s)) {
          st.error = *err;
          st.save_failed = true;
          st.notice = Notice::kNone;
          if (options.max_frames) {  // test: no user who could retry
            why = st.error;
            result = Result::kError;
            break;
          }
        } else {
          result = Result::kPlay;
          break;
        }
      }
    }

    extraction.CancelAndJoin();  // X or Quit during extraction: no marker is written
    if (install.valid() || dlss_download.valid()) optiscaler::CancelInstall();  // X, Quit or Play during the installation
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
  }
  DestroyWindow(hwnd);
  UnregisterClassW(kClass, inst);
  if (result == Result::kError) return fail(why);
  return result;
}

}  // namespace launcher
