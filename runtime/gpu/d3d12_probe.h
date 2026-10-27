// Search for a hardware D3D12 adapter, to decide whether the tests and the
// D3D12 backend can run (Global Constraints: D3D12 tests are skipped
// without a hardware adapter; WARP does not count). Same criterion as
// Xenia's D3D12Provider with d3d12_adapter=-1: the first DXGI adapter
// without DXGI_ADAPTER_FLAG_SOFTWARE on which D3D12CreateDevice with feature level
// 11_0 succeeds.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace gpu {

// Description (UTF-8) of the first non-software adapter with D3D12 FL 11_0;
// nullopt if there is none, if dxgi.dll/d3d12.dll are missing or if a call
// fails. Does not throw; loads and unloads the DLLs on every call.
std::optional<std::string> FindHardwareAdapter();

// Adapter that Xenia's D3D12Provider::Initialize would pick with
// d3d12_adapter = `selector`: >= 0 the adapter with that DXGI index (if it
// supports FL 11_0), -2 the first software one (WARP), otherwise the first
// hardware one (= FindHardwareAdapter). nullopt if none matches. Used to
// reject the configuration before D3D12Provider::Create, which on
// failure calls xe::FatalError.
std::optional<std::string> FindAdapter(int32_t selector);

}  // namespace gpu
