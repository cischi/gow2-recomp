// See d3d12_probe.h. The DLLs are loaded at runtime like Xenia's D3D12Provider does
// (D3D12Provider::Initialize), so the executable starts even where
// d3d12.dll is missing: in that case there is no adapter.
#include "gpu/d3d12_probe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>

#include <filesystem>
#include <string>

namespace gpu {

namespace {

// Frees the DLL when leaving the scope.
struct Library {
  HMODULE handle;
  explicit Library(const wchar_t* name) : handle(LoadLibraryW(name)) {}
  ~Library() {
    if (handle) FreeLibrary(handle);
  }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  template <class F>
  F Proc(const char* name) const {
    return handle ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(handle, name))) : nullptr;
  }
};

using PfnCreateDxgiFactory1 = HRESULT(WINAPI*)(REFIID, void**);

std::string Utf8(const wchar_t* text) {
  try {
    const auto u8 = std::filesystem::path(text).u8string();
    return std::string(u8.begin(), u8.end());
  } catch (...) {
    return "?";
  }
}

}  // namespace

std::optional<std::string> FindAdapter(int32_t selector) {
  try {
    Library dxgi(L"dxgi.dll");
    Library d3d12(L"d3d12.dll");
    const auto create_factory = dxgi.Proc<PfnCreateDxgiFactory1>("CreateDXGIFactory1");
    const auto create_device = d3d12.Proc<PFN_D3D12_CREATE_DEVICE>("D3D12CreateDevice");
    if (!create_factory || !create_device) return std::nullopt;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
      return std::nullopt;
    }
    std::optional<std::string> found;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      // Same order of checks as Xenia (D3D12Provider::Initialize). With
      // riid ID3D12Device and ppDevice nullptr the call only checks
      // that the device can be created.
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        const bool match = selector >= 0 ? i == UINT(selector) : selector == -2 ? software : !software;
        if (match) {
          desc.Description[std::size(desc.Description) - 1] = L'\0';
          found = Utf8(desc.Description);
        }
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
    return found;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> FindHardwareAdapter() { return FindAdapter(-1); }

}  // namespace gpu
