// Win32 + Direct3D 11 window plumbing shared by the launcher and gears.exe.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <functional>
#include <string>

namespace launcher {

struct D3D {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
};

// Panel scale and client size in px, chosen by the program from the DPI scale and the room on the monitor.
struct Placement {
  float scale;
  int client_w, client_h;
};
using FitFn = std::function<Placement(float dpi_scale, int work_h, int frame_h)>;

// Window sized by `fit` (client area), centered in the work area of the window's monitor. Returns the scale.
float PlaceWindow(HWND hwnd, DWORD style, UINT dpi, const FitFn& fit);

bool CreateD3D(HWND hwnd, D3D& d, std::string& why);

}  // namespace launcher
