// Frame generation: one generated frame between two real ones; the presenter draws into the generator's buffers and presents
// through it. FSR 3 (frame_generation_fsr.cpp, FidelityFX SDK v2.3.0): forwarding to the proxy swap chain. DLSS
// (frame_generation_dlss.cpp, gow2_dlss.dll bridge): its own texture ring and Present of the real swap chain from its
// timing thread. All methods are called from the presenter's Present thread.
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include "gpu/frame_generation_state.h"
#include "gpu/upscale_passes.h"

namespace gpu {

// Inputs of a generating frame. depth R32_FLOAT and motion R16G16_FLOAT at render resolution (conventions
// of upscaler.h), hudless R10G10B10A2 at output; all in NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE.
struct FrameGenInputs {
  ID3D12GraphicsCommandList* cmd;
  ID3D12Resource *depth, *motion, *hudless;
  uint32_t render_w, render_h;
  float jitter_x, jitter_y;
  float hdr_peak_nits;  // > 0: back buffer and hudless in PQ BT.2020; 0 = SDR
  // Constants captured for the inputs (camera matrices); valid only during Frame: the generator copies.
  const UberPostConstants* k = nullptr;
};

struct SwapChainRequest {
  ID3D12Device* device;
  IDXGIFactory2* factory;
  ID3D12CommandQueue* queue;  // provider's direct queue (gameQueue)
  HWND hwnd;
  DXGI_SWAP_CHAIN_DESC1 desc;
};

class FrameGenerator {
 public:
  virtual ~FrameGenerator() = default;  // after Shutdown
  // Once per Present, before Present() and after CurrentBufferIndex(). in != nullptr if and only if f.generate.
  // With in, the current buffer (GetBuffer(CurrentBufferIndex())) holds in in->cmd the finished image of the frame (HUD
  // included, overlay excluded) and is in RENDER_TARGET. FSR: context (re)created if needed, prepare in in->cmd and
  // configure on; without in: configure off. DLSS: with in, evaluate in in->cmd.
  virtual void Frame(IDXGISwapChain4* swap_chain, uint32_t display_w, uint32_t display_h, const FrameGenFrame& f,
                     const FrameGenInputs* in) = 0;
  // Buffer i (0..request's BufferCount - 1) the presenter draws into, resting state PRESENT; dimensions
  // and format those currently of the swap chain (DLSS: the ring is recreated here if they change).
  virtual HRESULT GetBuffer(UINT i, ID3D12Resource** out) = 0;
  // Buffer to draw in this frame. DLSS: waits for the timing thread to finish with that buffer.
  virtual UINT CurrentBufferIndex() = 0;
  // In place of swap_chain->Present, after the frame's ExecuteCommandLists on the request's queue.
  virtual HRESULT Present(UINT sync_interval, UINT flags) = 0;
  // Before ResizeBuffers and closing: on return no Present in flight and no reference (also on the
  // GPU) of the generator to the real swap chain's back buffers; the presenter then releases its own.
  virtual void WaitForPresents() = 0;
  // Shutdown order required by the frame generation API documentation; swap_chain released in here.
  virtual void Shutdown(Microsoft::WRL::ComPtr<IDXGISwapChain4>& swap_chain) = 0;
};

// Swap chain in `out` (FSR proxy or real DLSS one) and generator; nullptr (empty out, [fg] line with the reason) if not
// available.
using FrameGeneratorFactory = std::function<std::unique_ptr<FrameGenerator>(
    const SwapChainRequest&, Microsoft::WRL::ComPtr<IDXGISwapChain4>& out)>;

// Loader searched only in dll_dir (the gow2.exe folder in the game).
std::unique_ptr<FrameGenerator> CreateFsrFrameGenerator(const SwapChainRequest&,
                                                        Microsoft::WRL::ComPtr<IDXGISwapChain4>& out,
                                                        const std::filesystem::path& dll_dir);

// gow2_dlss.dll (ABI >= 3) searched only in dll_dir; `out` is the real swap chain (3 buffers, flags of the request).
std::unique_ptr<FrameGenerator> CreateDlssFrameGenerator(const SwapChainRequest&,
                                                         Microsoft::WRL::ComPtr<IDXGISwapChain4>& out,
                                                         const std::filesystem::path& dll_dir);

}  // namespace gpu
