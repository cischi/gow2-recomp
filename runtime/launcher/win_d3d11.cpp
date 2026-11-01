#include "launcher/win_d3d11.h"

#include <cstdio>

namespace launcher {

float PlaceWindow(HWND hwnd, DWORD style, UINT dpi, const FitFn& fit) {
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
  const int work_h = mi.rcWork.bottom - mi.rcWork.top, frame_h = frame.bottom - frame.top;
  const Placement p = fit(float(dpi) / 96, work_h, frame_h);
  RECT r{0, 0, p.client_w, p.client_h};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
  const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
  SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  return p.scale;
}

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
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
  if (FAILED(d.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
      FAILED(d.device->CreateRenderTargetView(back.Get(), nullptr, &d.rtv))) {
    why = "launcher back buffer unavailable";
    return false;
  }
  return true;
}

}  // namespace launcher
