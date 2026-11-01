// gears.exe: small window to pick a game; starts its exe (which opens its own launcher) and exits.
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <cfloat>
#include <cwctype>
#include <string>

#include "dispatcher/dispatcher_model.h"
#include "launcher/imgui_style.h"
#include "launcher/win_d3d11.h"
#include "platform/platform.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kClass[] = L"gears_dispatcher";
constexpr int kWidth = 420, kHeight = 260;

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

// DPI scale, reduced to fit the work area (never below 0.5).
launcher::Placement Fit(float dpi_scale, int work_h, int frame_h) {
  float scale = dpi_scale;
  if (kHeight * scale + frame_h > work_h) scale = float(work_h - frame_h) / kHeight;
  if (scale < 0.5f) scale = 0.5f;
  return {scale, int(kWidth * scale), int(kHeight * scale)};
}

// Starts exe with its own folder as the working directory. 0 on success, else GetLastError.
DWORD Launch(const std::filesystem::path& exe) {
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si,
                      &pi))
    return GetLastError();
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);  // see the delay load in CMakeLists.txt
  unsigned long max_frames = 0;  // test only: exit 0 after this many frames
  for (int i = 1; i < argc; ++i) {
    if (std::wstring(argv[i]) == L"--max-frames" && i + 1 < argc) {
      wchar_t* end = nullptr;
      max_frames = wcstoul(argv[++i], &end, 10);
      if (!iswdigit(argv[i][0]) || *end != L'\0' || max_frames == 0) return 2;
    } else {
      return 2;
    }
  }

  const bool italian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
  const auto games = dispatcher::FindGames(plat::ExecutablePath().parent_path());

  plat::EnablePerMonitorDpiAwareness();  // before any window: otherwise Windows bitmap-scales it
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
  wc.lpszClassName = kClass;
  if (!RegisterClassExW(&wc)) return 1;
  constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  HWND hwnd = CreateWindowExW(0, kClass, L"Gears of War", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
                              nullptr, nullptr, inst, nullptr);
  if (!hwnd) {
    UnregisterClassW(kClass, inst);
    return 1;
  }
  UINT dpi = GetDpiForWindow(hwnd);
  const float scale = launcher::PlaceWindow(hwnd, kStyle, dpi, Fit);

  {
    launcher::D3D d;
    std::string why;
    if (!launcher::CreateD3D(hwnd, d, why)) {
      MessageBoxA(hwnd, why.c_str(), "Gears of War", MB_OK | MB_ICONERROR);
      DestroyWindow(hwnd);
      UnregisterClassW(kClass, inst);
      return 1;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    launcher::LoadFonts();
    launcher::ApplyScale(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d.device.Get(), d.context.Get());

    std::string message;
    for (unsigned long frame = 1;; ++frame) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (g_close || (max_frames && frame > max_frames)) break;
      if (g_dpi_changed) {
        dpi = g_dpi_changed;
        g_dpi_changed = 0;
        launcher::ApplyScale(launcher::PlaceWindow(hwnd, kStyle, dpi, Fit));
        d.rtv.Reset();
        d.swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
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
      bool done = false;
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(io.DisplaySize);
      ImGui::Begin("gears", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      ImGui::TextUnformatted("Gears of War");
      ImGui::Separator();
      for (const auto& e : games) {
        const std::string id(e.game.id);
        ImGui::BeginDisabled(!e.available);
        const bool clicked = ImGui::Button((std::string(e.game.name) + "###" + id).c_str(), ImVec2(-FLT_MIN, 0));
        ImGui::EndDisabled();
        if (!e.available) {
          ImGui::TextDisabled("%s", (id + (italian ? ".exe non trovato" : ".exe not found")).c_str());
        } else if (clicked) {
          if (const DWORD err = Launch(e.exe))
            message = (italian ? "Avvio non riuscito (errore " : "Launch failed (error ") + std::to_string(err) + ")";
          else
            done = true;
        }
      }
      if (ImGui::Button(italian ? "Esci" : "Quit", ImVec2(-FLT_MIN, 0))) done = true;
      if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
      ImGui::End();
      ImGui::Render();
      static constexpr float kClear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
      if (d.rtv) {
        d.context->OMSetRenderTargets(1, d.rtv.GetAddressOf(), nullptr);
        d.context->ClearRenderTargetView(d.rtv.Get(), kClear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      }
      d.swap_chain->Present(1, 0);
      if (done) break;
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
  }
  DestroyWindow(hwnd);
  UnregisterClassW(kClass, inst);
  return 0;
}
