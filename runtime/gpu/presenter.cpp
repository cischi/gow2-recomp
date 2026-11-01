// See presenter.h. D3D12/DXGI APIs and GetClientRect on the window (arrives
// as a handle): exception to the Global Constraints for runtime/gpu.
#include "gpu/presenter.h"

#include "gpu/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <bit>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <psapi.h>  // K32GetProcessMemoryInfo for the [perf] line

#include "gpu/bmp.h"
#include "gpu/gpu_system.h"
#include "kernel/stats.h"
#include "log.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace gpu::shaders {
// Xenia DXBC bytecode (vendored, see thirdparty/xenia/VENDORED.md).
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"
}  // namespace gpu::shaders

namespace gpu {

namespace {

using Microsoft::WRL::ComPtr;

std::string Show(const std::filesystem::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

void LogHr(const char* what, HRESULT hr) {
  char line[160];
  std::snprintf(line, sizeof(line), "[gpu] presenter: %s failed (0x%08X)", what, unsigned(hr));
  LogLine(line);
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

// 10-bit channel -> 8 bit (v >> 2), R in bits 0..9 of R10G10B10A2.
uint32_t ToXrgb8888(uint32_t r10g10b10a2) {
  const uint32_t r = (r10g10b10a2 & 0x3FF) >> 2;
  const uint32_t g = ((r10g10b10a2 >> 10) & 0x3FF) >> 2;
  const uint32_t b = ((r10g10b10a2 >> 20) & 0x3FF) >> 2;
  return r << 16 | g << 8 | b;
}

}  // namespace

Rect FitRect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, bool stretch) {
  if (!src_w || !src_h || !dst_w || !dst_h) return {0, 0, 0, 0};
  if (stretch) return {0, 0, dst_w, dst_h};
  const uint32_t h = uint32_t(uint64_t(dst_w) * src_h / src_w);
  if (h <= dst_h) return {0, (dst_h - h) / 2, dst_w, h};
  const uint32_t w = uint32_t(uint64_t(dst_h) * src_w / src_h);
  return {(dst_w - w) / 2, 0, w, dst_h};
}

PaintEffect ChoosePaintEffect(uint32_t src_w, uint32_t src_h, Rect dst, bool sharpening) {
  if (!sharpening) return PaintEffect::kBilinear;
  if (dst.w == src_w && dst.h == src_h) return PaintEffect::kCasSharpen;
  const auto within_2x = [](uint64_t s, uint64_t d) { return s <= 2 * d && d <= 2 * s; };
  return within_2x(src_w, dst.w) && within_2x(src_h, dst.h) ? PaintEffect::kCasResample : PaintEffect::kBilinear;
}

std::unique_ptr<Presenter> Presenter::Create(const xe::ui::d3d12::D3D12Provider& provider, void* hwnd,
                                             const PresentOptions& options) {
  std::unique_ptr<Presenter> presenter(new Presenter(provider));
  if (!presenter->Initialize(hwnd, options)) return nullptr;
  return presenter;
}

bool Presenter::Initialize(void* hwnd, const PresentOptions& options) {
  ID3D12Device* device = provider_.GetDevice();
  if (!hwnd || !device) return false;
  hwnd_ = hwnd;
  options_ = options;
  {  // also with vsync: SetVideo can turn it off hot
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory5))) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) &&
        tearing)
      swap_chain_flags_ = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = options.width;
  desc.Height = options.height;
  // 0 x 0 would be fine for DXGI (client area), but OptiScaler mistakes swap chains under 100 px for overlays
  // and does not hook its menu on them: the explicit client area is passed.
  RECT client{};
  if ((!desc.Width || !desc.Height) && GetClientRect(static_cast<HWND>(hwnd), &client) && client.right > 0 &&
      client.bottom > 0)
    desc.Width = UINT(client.right), desc.Height = UINT(client.bottom);
  // Without ALLOW_MODE_SWITCH exclusive stays at the desktop resolution. Always: the overlay can
  // switch to exclusive fullscreen hot (outside exclusive the flag has no effect).
  swap_chain_flags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  desc.Flags = swap_chain_flags_;
  desc.Format = kGuestOutputFormat;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = kBufferCount;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (options.frame_generator) {
    frame_gen_ = options.frame_generator(
        {device, provider_.GetDXGIFactory(), provider_.GetDirectQueue(), static_cast<HWND>(hwnd), desc}, swap_chain_);
    if (frame_gen_) {
      if (!upscale_passes_ && !(upscale_passes_ = UpscalePasses::Create(device)))
        LogLine("[fg] frame generation passes not created: no frame generated");
      frame_gen_enabled_ = true;
      RefuseExclusiveWithFrameGen(options_.exclusive_fullscreen);
    }  // otherwise the line with the reason was written by the factory (frame_generation.h)
  }
  HRESULT hr = S_OK;
  if (!frame_gen_) {
    ComPtr<IDXGISwapChain1> swap_chain1;
    hr = provider_.GetDXGIFactory()->CreateSwapChainForHwnd(provider_.GetDirectQueue(), static_cast<HWND>(hwnd),
                                                            &desc, nullptr, nullptr, &swap_chain1);
    if (FAILED(hr)) return LogHr("CreateSwapChainForHwnd", hr), false;
    if (FAILED(hr = swap_chain1.As(&swap_chain_))) return LogHr("IDXGISwapChain4", hr), false;
  }
  if (options.hdr) {  // after the swap chain, normal or proxy
    constexpr auto kPq = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    UINT support = 0;
    HRESULT cs = swap_chain_->CheckColorSpaceSupport(kPq, &support);
    if (SUCCEEDED(cs) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
      cs = swap_chain_->SetColorSpace1(kPq);
    else if (SUCCEEDED(cs))
      cs = DXGI_ERROR_UNSUPPORTED;
    if (FAILED(cs)) {
      char line[128];
      std::snprintf(line, sizeof line, "[hdr] HDR10 color space not accepted (support 0x%X, 0x%08X): SDR",
                    support, unsigned(cs));
      LogLine(line);
    } else if (!(hdr_passes_ = HdrPasses::Create(device))) {  // Create wrote the line
      swap_chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
      hdr_levels_ = options.hdr;
    }
  }
  // Alt-Enter disabled: no exclusive fullscreen decided by DXGI.
  provider_.GetDXGIFactory()->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);

  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = HdrActive() ? 2 * kBufferCount + 1 : kBufferCount;  // + hdr_sdr_, hdr_hudless_
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_))))
    return LogHr("CreateDescriptorHeap", hr), false;
  rtv_size_ = provider_.GetRTVDescriptorSize();
  if (!AcquireBuffers()) return false;
  for (uint32_t i = 0; i < kBufferCount; ++i) {
    if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]))))
      return LogHr("CreateCommandAllocator", hr), false;
  }
  if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
                                            IID_PPV_ARGS(&list_))))
    return LogHr("CreateCommandList", hr), false;
  list_->Close();
  if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
    return LogHr("CreateFence", hr), false;
  return CreatePaintPipeline();
}

bool Presenter::AcquireBuffers() {
  DXGI_SWAP_CHAIN_DESC1 desc{};
  HRESULT hr = swap_chain_->GetDesc1(&desc);
  if (FAILED(hr)) return LogHr("GetDesc1", hr), false;
  width_ = desc.Width;
  height_ = desc.Height;
  for (uint32_t i = 0; i < kBufferCount; ++i) {
    hr = frame_gen_ ? frame_gen_->GetBuffer(i, back_buffers_[i].ReleaseAndGetAddressOf())
                    : swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
    if (FAILED(hr))
      return LogHr("GetBuffer", hr), false;
    provider_.GetDevice()->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, Rtv(i));
  }
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Presenter::Rtv(uint32_t i) const {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += size_t(i) * rtv_size_;
  return rtv;
}

bool Presenter::CreatePaintPipeline() {
  ID3D12Device* device = provider_.GetDevice();
  // Like Xenia's D3D12Presenter (GuestOutputPaintRootParameter): source SRV
  // (t0, pixel), rectangle constants (b0, vertex), effect
  // constants (b0, pixel: BilinearConstants 4 values, CasSharpenConstants 3,
  // CasResampleConstants 5); static bilinear sampler (s0) only for kBilinear
  // (CAS reads texels without a sampler). Indices like PaintEffect.
  struct Effect {
    UINT constants;
    D3D12_SHADER_BYTECODE ps;
  };
  const Effect effects[3] = {
      {4, {shaders::guest_output_bilinear_ps, sizeof(shaders::guest_output_bilinear_ps)}},
      {3, {shaders::guest_output_ffx_cas_sharpen_ps, sizeof(shaders::guest_output_ffx_cas_sharpen_ps)}},
      {5, {shaders::guest_output_ffx_cas_resample_ps, sizeof(shaders::guest_output_ffx_cas_resample_ps)}}};
  const D3D12_DESCRIPTOR_RANGE source{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &source};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 4};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.VS = {shaders::guest_output_triangle_strip_rect_vs, sizeof(shaders::guest_output_triangle_strip_rect_vs)};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = kGuestOutputFormat;
  pso.SampleDesc.Count = 1;
  HRESULT hr = S_OK;
  for (size_t i = 0; i < std::size(effects); ++i) {
    params[2].Constants = {0, 0, effects[i].constants};
    const bool bilinear = PaintEffect(i) == PaintEffect::kBilinear;
    const D3D12_ROOT_SIGNATURE_DESC root{3, params, bilinear ? 1u : 0u, bilinear ? &sampler : nullptr,
                                         D3D12_ROOT_SIGNATURE_FLAG_NONE};
    paint_root_signatures_[i].Attach(xe::ui::d3d12::util::CreateRootSignature(provider_, root));
    if (!paint_root_signatures_[i])
      return LogLine("[gpu] presenter: guest output draw root signature not created"), false;
    pso.pRootSignature = paint_root_signatures_[i].Get();
    pso.PS = effects[i].ps;
    if (FAILED(hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&paint_pipelines_[i]))))
      return LogHr("CreateGraphicsPipelineState", hr), false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = 2;  // kSrvGuestOutput, kSrvUpscaled
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_))))
    return LogHr("CreateDescriptorHeap (SRV)", hr), false;
  srv_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

std::pair<uint32_t, uint32_t> Presenter::ClientArea() const {
  RECT r{};
  if (!GetClientRect(static_cast<HWND>(hwnd_), &r)) return {0, 0};
  return {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
}

bool Presenter::ResizeIfNeeded(uint32_t client_w, uint32_t client_h) {
  if ((client_w == width_ && client_h == height_) || std::pair{client_w, client_h} == resize_failed_) return true;
  WaitIdle();  // the back buffers must no longer be in use
  if (frame_gen_) frame_gen_->WaitForPresents();  // nor by the proxy
  for (auto& buffer : back_buffers_) buffer.Reset();
  readback_.Reset();  // the footprint depends on the dimensions: recreated at the next screenshot
  // They follow the back buffer: recreated at the next use (GPU and proxy idle above).
  readback_pq_.Reset();
  hdr_sdr_.Reset();
  hdr_pre_overlay_.Reset();
  for (auto& r : hdr_hudless_) r.Reset();
  // 0 = number of buffers unchanged (DLSS FG's real swap chain has 3, not kBufferCount).
  const HRESULT hr = swap_chain_->ResizeBuffers(0, client_w, client_h, DXGI_FORMAT_UNKNOWN, swap_chain_flags_);
  if (FAILED(hr)) {
    // The previous size is kept (DXGI_SCALING_STRETCH fits it).
    LogHr("ResizeBuffers", hr);
    resize_failed_ = {client_w, client_h};
  } else {
    resize_failed_ = {};
    if (HdrActive() && FAILED(swap_chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)))
      DisableHdr("HDR10 color space rejected after the resize");
  }
  const bool ok = AcquireBuffers();
  if ((FAILED(hr) || !ok) && provider_.GetDevice()->GetDeviceRemovedReason() != S_OK) LogDeviceRemoved();
  return ok;
}

void Presenter::DrawGuestOutput(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t srv, uint32_t src_w, uint32_t src_h) {
  static_assert(kGuestOutputInternalState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "the guest output is sampled in the state the refresher leaves it in");
  const Rect r = FitRect(src_w, src_h, width_, height_, options_.stretch);
  if (!r.w || !r.h) return;
  const PaintEffect effect = ChoosePaintEffect(src_w, src_h, r, options_.sharpening);
  if (options_.sharpening && effect == PaintEffect::kBilinear && !cas_fallback_logged_) {
    cas_fallback_logged_ = true;
    LogLine("[gpu] CAS: rectangle " + std::to_string(r.w) + "x" + std::to_string(r.h) +
            " outside the 1/2..2x ratio of the guest output, bilinear without sharpening");
  }
  const size_t e = size_t(effect);
  list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{float(r.x), float(r.y), float(r.w), float(r.h), 0.0f, 1.0f};
  list_->RSSetViewports(1, &viewport);
  const D3D12_RECT scissor{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
  list_->RSSetScissorRects(1, &scissor);
  list_->SetPipelineState(paint_pipelines_[e].Get());
  list_->SetGraphicsRootSignature(paint_root_signatures_[e].Get());
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += uint64_t(srv) * srv_size_;
  list_->SetGraphicsRootDescriptorTable(0, table);
  // GuestOutputPaintRectangleConstants in NDC: the viewport is already the
  // rectangle, so it covers it entirely (+Y is -V).
  const float rect[4] = {-1.0f, 1.0f, 2.0f, -2.0f};
  list_->SetGraphicsRoot32BitConstants(1, 4, rect, 0);
  // Effect constants (Xenia's presenter.h), all headed by the origin
  // of the rectangle in back buffer pixels (int32_t, here the same bits). Bilinear: 1 / dimensions. CAS:
  // CalculateCasPostSetupSharpness and, in resample, source / destination.
  const float post_setup = -1.0f / (8.0f - 3.0f * options_.sharpness);
  const float sw = float(src_w), sh = float(src_h), dw = float(r.w), dh = float(r.h);
  const uint32_t bits[3][5] = {
      {r.x, r.y, std::bit_cast<uint32_t>(1.0f / dw), std::bit_cast<uint32_t>(1.0f / dh)},
      {r.x, r.y, std::bit_cast<uint32_t>(post_setup)},
      {r.x, r.y, std::bit_cast<uint32_t>(sw / dw), std::bit_cast<uint32_t>(sh / dh),
       std::bit_cast<uint32_t>(post_setup)}};
  constexpr UINT kCount[3] = {4, 3, 5};
  list_->SetGraphicsRoot32BitConstants(2, kCount[e], bits[e], 0);
  list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  list_->DrawInstanced(4, 1, 0, 0);
}

Presenter::~Presenter() {
  if (fence_) WaitIdle();
  // DXGI does not accept releasing a swap chain in exclusive fullscreen.
  if (swap_chain_ && exclusive_) swap_chain_->SetFullscreenState(FALSE, nullptr);
  if (frame_gen_) {
    for (auto& buffer : back_buffers_) buffer.Reset();
    frame_gen_->Shutdown(swap_chain_);
  }
}

void Presenter::RefuseExclusiveWithFrameGen(bool& exclusive) {
  if (!exclusive) return;
  exclusive = false;
  if (fg_exclusive_logged_) return;
  fg_exclusive_logged_ = true;
  LogLine("[fg] exclusive fullscreen not allowed with frame generation: borderless");
}

bool Presenter::EnterFullscreen() {
  ComPtr<IDXGIOutput> output;
  HRESULT hr = swap_chain_->GetContainingOutput(&output);
  if (FAILED(hr)) return LogHr("GetContainingOutput", hr), false;
  DXGI_OUTPUT_DESC out{};
  output->GetDesc(&out);
  // Current mode of the output (default dimensions and frequency).
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(out.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
    dm.dmPelsWidth = DWORD(out.DesktopCoordinates.right - out.DesktopCoordinates.left);
    dm.dmPelsHeight = DWORD(out.DesktopCoordinates.bottom - out.DesktopCoordinates.top);
    dm.dmDisplayFrequency = 0;
  }
  DXGI_MODE_DESC want{};
  want.Width = options_.mode_width ? options_.mode_width : dm.dmPelsWidth;
  want.Height = options_.mode_height ? options_.mode_height : dm.dmPelsHeight;
  if (dm.dmDisplayFrequency > 1) want.RefreshRate = {dm.dmDisplayFrequency, 1};
  want.Format = kGuestOutputFormat;
  DXGI_MODE_DESC mode{};
  if (FAILED(hr = output->FindClosestMatchingMode(&want, &mode, nullptr)))
    return LogHr("FindClosestMatchingMode", hr), false;
  // FindClosestMatchingMode can answer STRETCHED even for modes listed
  // only as UNSPECIFIED/CENTERED: ResizeTarget would pick another mode
  // (seen: 1920x1080 -> 1920x1440).
  mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  hr = swap_chain_->SetFullscreenState(TRUE, nullptr);
  if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) return true;  // retried
  if (FAILED(hr)) return LogHr("SetFullscreenState(TRUE)", hr), false;
  // The mode is changed from exclusive (ALLOW_MODE_SWITCH): from windowed ResizeTarget
  // would only resize the window.
  if (FAILED(hr = swap_chain_->ResizeTarget(&mode))) {
    LogHr("ResizeTarget", hr);
    swap_chain_->SetFullscreenState(FALSE, nullptr);
    return false;
  }
  char line[96];
  std::snprintf(line, sizeof(line), "[gpu] exclusive fullscreen %ux%u %.2f Hz", mode.Width, mode.Height,
                mode.RefreshRate.Denominator ? double(mode.RefreshRate.Numerator) / mode.RefreshRate.Denominator : 0.0);
  LogLine(line);
  return true;
}

void Presenter::UpdateFullscreen() {
  if (frame_gen_) return;  // the proxy stays borderless
  BOOL fullscreen = FALSE;
  swap_chain_->GetFullscreenState(&fullscreen, nullptr);
  const bool wanted = !options_.exclusive_wanted || options_.exclusive_wanted(fullscreen);
  if (wanted && !fullscreen && !exclusive_failed_) {
    // The window must know first: an Alt-Enter during the entry is deferred to the exit.
    if (options_.exclusive_wanted) options_.exclusive_wanted(true);
    if (!EnterFullscreen()) {
      LogLine("[gpu] exclusive fullscreen not available: staying borderless");
      exclusive_failed_ = true;
    }
  } else if (!wanted && fullscreen) {
    const HRESULT hr = swap_chain_->SetFullscreenState(FALSE, nullptr);
    if (FAILED(hr)) LogHr("SetFullscreenState(FALSE)", hr);
  }
  swap_chain_->GetFullscreenState(&fullscreen, nullptr);
  if (bool(fullscreen) != exclusive_) {
    exclusive_ = fullscreen;
    if (!exclusive_) LogLine(wanted ? "[gpu] exclusive fullscreen lost" : "[gpu] exclusive fullscreen left");
    // After every state change DXGI wants ResizeBuffers, even with the same dimensions.
    width_ = height_ = 0;
    resize_failed_ = {};
    if (!exclusive_ && options_.exclusive_wanted) options_.exclusive_wanted(false);  // the window knows immediately
  }
}

uint64_t Presenter::Signal() {
  provider_.GetDirectQueue()->Signal(fence_.Get(), ++fence_value_);
  return fence_value_;
}

void Presenter::WaitFor(uint64_t value) {
  // With the device lost the fence is UINT64_MAX: returns immediately.
  if (fence_->GetCompletedValue() < value) fence_->SetEventOnCompletion(value, nullptr);
}

bool Presenter::RefreshGuestOutput(uint32_t fb_w, uint32_t fb_h, uint32_t /*ax*/, uint32_t /*ay*/,
                                   std::function<bool(GuestOutputRefreshContext&)> refresher) {
  if (!fb_w || !fb_h || !refresher) return false;
  if (!guest_output_ || guest_width_ != fb_w || guest_height_ != fb_h) {
    // The previous texture may still be in use by a Present's copy.
    if (guest_output_) WaitIdle();
    guest_output_.Reset();
    guest_output_valid_ = false;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = fb_w;
    desc.Height = fb_h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = provider_.GetDevice()->CreateCommittedResource(
        &heap, provider_.GetHeapFlagCreateNotZeroed(), &desc, kGuestOutputInternalState, nullptr,
        IID_PPV_ARGS(&guest_output_));
    if (FAILED(hr)) {
      LogHr("CreateCommittedResource (guest output)", hr);
      return false;
    }
    guest_width_ = fb_w;
    guest_height_ = fb_h;
    // The previous texture is no longer in use (WaitIdle above): the SRV is rewritten.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kGuestOutputFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    provider_.GetDevice()->CreateShaderResourceView(guest_output_.Get(), &srv,
                                                    srv_heap_->GetCPUDescriptorHandleForHeapStart());
  }
  bool is_8bpc = false;
  D3D12GuestOutputRefreshContext context(is_8bpc, guest_output_.Get());
  const bool ok = refresher(context);
  // A failed refresher may have left the texture half-written: it is not presented
  // (black) until a refresh succeeds.
  guest_output_valid_ = ok;
  if (ok) ++refresh_count_;
  return ok;
}

void Presenter::ArmScreenshot(std::chrono::steady_clock::time_point at, std::filesystem::path file) {
  std::lock_guard g(screenshot_mutex_);
  const auto pos = std::upper_bound(screenshots_.begin(), screenshots_.end(), at,
                                    [](const auto& t, const auto& item) { return t < item.first; });
  screenshots_.emplace(pos, at, std::move(file));
}

bool Presenter::EnsureReadback() {
  if (readback_ && (readback_pq_ || !HdrActive())) return true;
  const D3D12_RESOURCE_DESC desc = back_buffers_[0]->GetDesc();
  UINT64 total = 0;
  provider_.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &readback_footprint_, nullptr, nullptr, &total);
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // With HDR also the PQ back buffer readback (same footprint: hdr_sdr_ has the back buffer's format and dimensions).
  const auto create = [&](ComPtr<ID3D12Resource>& r) {
    if (r) return true;
    const HRESULT hr = provider_.GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) LogHr("CreateCommittedResource (readback)", hr);
    return SUCCEEDED(hr);
  };
  return create(readback_) && (!HdrActive() || create(readback_pq_));
}

bool Presenter::EnsureHdrSdr() {
  if (hdr_sdr_) return true;
  D3D12_RESOURCE_DESC desc = back_buffers_[0]->GetDesc();
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const HRESULT hr = provider_.GetDevice()->CreateCommittedResource(&xe::ui::d3d12::util::kHeapPropertiesDefault,
                                                                    D3D12_HEAP_FLAG_NONE, &desc,
                                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                    IID_PPV_ARGS(&hdr_sdr_));
  if (FAILED(hr)) return DisableHdr("intermediate SDR image not created"), false;
  provider_.GetDevice()->CreateRenderTargetView(hdr_sdr_.Get(), nullptr, Rtv(kBufferCount));
  return true;
}

void Presenter::DisableHdr(const char* why) {
  if (!hdr_levels_) return;  // a second error in the same frame (local variable hdr still true): a single line
  LogLine(std::string("[hdr] ") + why + ": SDR");
  hdr_levels_.reset();
  swap_chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
}

void Presenter::LogDeviceRemoved() {
  LogDeviceRemovedOnce(uint32_t(provider_.GetDevice()->GetDeviceRemovedReason()));
}

void Presenter::SetUpscaler(UpscalerFactory factory, float sharpness, UpscaleDebug debug) {
  upscale_passes_ = UpscalePasses::Create(provider_.GetDevice());
  upscaler_factory_ = upscale_passes_ ? std::move(factory) : nullptr;
  upscale_sharpness_ = sharpness;
  upscale_debug_ = debug;
}

void Presenter::SetUpscaleCapture(const UpscaleCapture* capture) {
  if (capture) {
    capture_ = *capture;
  } else {
    capture_.reset();
    upscale_state_.NoteFrameWithoutCapture();
  }
}

std::pair<float, float> Presenter::NextJitter() {
  if (!upscaler_factory_ || !upscaler_ || !jitter_render_w_ || !upscaled_last_) return {0.0f, 0.0f};
  const uint32_t phases = upscaler_->JitterPhaseCount(jitter_render_w_, jitter_output_w_);
  float x = 0.0f, y = 0.0f;
  upscaler_->JitterOffset(++jitter_index_ % phases, phases, x, y);
  return {x, y};
}

float Presenter::MipBias() const {
  if (!upscaler_factory_ || !upscaler_ || !jitter_render_w_ || !upscaled_last_) return 0.0f;
  return UpscalerMipBias(jitter_render_w_, jitter_output_w_);
}

bool Presenter::EnsureUpscaleTargets(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
  bool output_recreated = false;
  auto make = [&](ComPtr<ID3D12Resource>& r, uint32_t w, uint32_t h, DXGI_FORMAT format) {
    if (r && r->GetDesc().Width == w && r->GetDesc().Height == h) return true;
    // The previous frame may still be using it. The DLSS bridge also counts on this wait: it recreates its
    // feature when the dimensions change, i.e. only after this point.
    if (r) WaitIdle();
    r.Reset();
    if (&r == &upscaled_) output_recreated = true;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(provider_.GetDevice()->CreateCommittedResource(
        &xe::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r)));
  };
  if (!make(motion_, rw, rh, DXGI_FORMAT_R16G16_FLOAT) || !make(reactive_, rw, rh, DXGI_FORMAT_R8_UNORM) ||
      !make(upscaled_, ow, oh, kGuestOutputFormat) || !make(scene_in_, rw, rh, kGuestOutputFormat) ||
      !make(upscaled_scene_, ow, oh, kGuestOutputFormat)) {
    LogLine("[gpu] upscaler: textures " + std::to_string(ow) + "x" + std::to_string(oh) + " not created");
    return false;
  }
  // SRV rewritten on every recreation (the previous one is no longer in use: WaitIdle in make), even if the
  // new texture has the same pointer as the old one just freed: comparing pointers left
  // the SRV on freed memory (DXGI_ERROR_DEVICE_HUNG on Alt+Enter from fullscreen to windowed).
  if (output_recreated) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kGuestOutputFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(kSrvUpscaled) * srv_size_;
    provider_.GetDevice()->CreateShaderResourceView(upscaled_.Get(), &srv, h);
  }
  return true;
}

bool Presenter::DrawUpscaled(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t slot) {
  if (!upscaler_factory_) return false;
  // Turns the upscaler off forever (and the jitter with it): without reconstruction the jittered frames would shake.
  // upscaler_ stays alive as long as the GPU may use it (destroyed with the presenter, after WaitIdle).
  auto disable = [&](const std::string& why) {
    upscaler_factory_ = nullptr;
    LogLine("[gpu] upscaler off: " + why + "; continuing without");
    return false;
  };
  const UpscaleCapture& c = *capture_;
  const D3D12_RESOURCE_DESC pd = c.prehud->GetDesc();
  const uint32_t rw = uint32_t(pd.Width), rh = pd.Height;
  // The reactive mask compares the guest output with the captured image pixel by pixel.
  if (guest_width_ != rw || guest_height_ != rh)
    return disable("guest output " + std::to_string(guest_width_) + "x" + std::to_string(guest_height_) +
                   " differs from the capture " + std::to_string(rw) + "x" + std::to_string(rh));
  const Rect r = FitRect(rw, rh, width_, height_, options_.stretch);
  if (!r.w || !r.h) return false;
  const auto [ow, oh] = UpscaleOutputSize(rw, rh, r.w, r.h);
  if (!upscaler_ || rw > upscaler_max_[0] || rh > upscaler_max_[1] || ow > upscaler_max_[2] || oh > upscaler_max_[3]) {
    WaitIdle();
    upscaler_.reset();
    upscaler_ = upscaler_factory_(rw, rh, ow, oh);
    if (!upscaler_) {
      upscaler_factory_ = nullptr;
      LogLine("[gpu] upscaler not available (" + std::to_string(rw) + "x" + std::to_string(rh) + " -> " +
              std::to_string(ow) + "x" + std::to_string(oh) + "): off until restart, continuing without");
      return false;
    }
    upscaler_max_[0] = rw, upscaler_max_[1] = rh, upscaler_max_[2] = ow, upscaler_max_[3] = oh;
    LogLine("[gpu] upscaler: " + upscaler_->Name() + " (" + std::to_string(rw) + "x" + std::to_string(rh) + " -> " +
            std::to_string(ow) + "x" + std::to_string(oh) + ")");
  }
  if (!EnsureUpscaleTargets(rw, rh, ow, oh)) return disable("textures not created");
  const bool reset = upscale_state_.NeedsReset(rw, rh, ow, oh, c.k);
  const auto now = std::chrono::steady_clock::now();
  const float frame_ms = last_upscale_.time_since_epoch().count()
                             ? std::chrono::duration<float, std::milli>(now - last_upscale_).count()
                             : 33.3f;
  last_upscale_ = now;

  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Resource* inputs[] = {c.prehud.Get(), c.depth.Get(), c.velocity.Get()};
  D3D12_RESOURCE_BARRIER to_read[4], back[4];
  for (int i = 0; i < 3; ++i) {
    to_read[i] = Transition(inputs[i], D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    back[i] = Transition(inputs[i], kRead, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  to_read[3] = Transition(guest_output_.Get(), kGuestOutputInternalState, kRead);
  back[3] = Transition(guest_output_.Get(), kRead, kGuestOutputInternalState);
  list_->ResourceBarrier(4, to_read);
  upscale_passes_->MotionVectors(list_.Get(), c.depth.Get(), c.velocity.Get(), motion_.Get(), c.k, slot);
  upscale_passes_->ReactiveMask(list_.Get(), c.prehud.Get(), guest_output_.Get(), c.ramp, reactive_.Get(), slot);
  D3D12_RESOURCE_BARRIER passes_done[2] = {Transition(motion_.Get(), kUav, kRead),
                                           Transition(reactive_.Get(), kUav, kRead)};
  list_->ResourceBarrier(2, passes_done);
  // The upscaler receives the scene without HUD; the HUD returns on top afterwards (ComposeHud), from the guest output.
  upscale_passes_->SceneWithoutHud(list_.Get(), guest_output_.Get(), c.prehud.Get(), reactive_.Get(), c.ramp.pwl,
                                   scene_in_.Get(), slot);
  const D3D12_RESOURCE_BARRIER scene_ready = Transition(scene_in_.Get(), kUav, kRead);
  list_->ResourceBarrier(1, &scene_ready);
  UpscaleFrame f{list_.Get(), scene_in_.Get(), c.depth.Get(), motion_.Get(), upscaled_scene_.Get(),
                 reactive_.Get(), rw, rh, ow, oh, c.jitter_x, c.jitter_y, 1.0f, 1.0f, reset,
                 upscale_sharpness_, frame_ms, upscale_debug_ == UpscaleDebug::kFfxDebugView};
  const bool ok = upscaler_->Dispatch(f);
  if (ok) {
    const D3D12_RESOURCE_BARRIER scene_done = Transition(upscaled_scene_.Get(), kUav, kRead);
    list_->ResourceBarrier(1, &scene_done);
    upscale_passes_->ComposeHud(list_.Get(), upscaled_scene_.Get(), guest_output_.Get(), reactive_.Get(),
                                upscaled_.Get(), slot);
    const D3D12_RESOURCE_BARRIER scene_back = Transition(upscaled_scene_.Get(), kRead, kUav);
    list_->ResourceBarrier(1, &scene_back);
  }
  D3D12_RESOURCE_BARRIER after[4] = {
      Transition(motion_.Get(), kRead, kUav), Transition(reactive_.Get(), kRead, kUav),
      Transition(upscaled_.Get(), kUav, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      Transition(scene_in_.Get(), kRead, kUav)};
  list_->ResourceBarrier(4, after);
  list_->ResourceBarrier(4, back);
  if (ok) DrawGuestOutput(rtv, kSrvUpscaled, ow, oh);
  const D3D12_RESOURCE_BARRIER to_uav = Transition(upscaled_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kUav);
  list_->ResourceBarrier(1, &to_uav);
  jitter_render_w_ = rw;
  jitter_output_w_ = ow;
  return ok || disable("dispatch failed");
}

void Presenter::SetOverlay(std::unique_ptr<Overlay> overlay) { overlay_ = std::move(overlay); }

void Presenter::ApplyVideo(const VideoChange& c) {
  options_.vsync = c.vsync;
  options_.stretch = c.stretch;
  options_.sharpening = c.sharpening && !UpscalerActive();  // with the upscaler the sharpening is its RCAS
  options_.sharpness = c.sharpness;
  upscale_sharpness_ = c.sharpening ? c.sharpness : -1.0f;
  // Exclusive stays allowed once on: exclusive_wanted then decides (window
  // mode), so Alt-Enter from windowed returns to exclusive fullscreen as at startup.
  bool exclusive = options_.exclusive_fullscreen || c.exclusive_fullscreen;
  if (frame_gen_) RefuseExclusiveWithFrameGen(exclusive);
  const bool mode_changed = exclusive != options_.exclusive_fullscreen || c.mode_width != options_.mode_width ||
                            c.mode_height != options_.mode_height;
  options_.exclusive_fullscreen = exclusive;
  options_.mode_width = c.mode_width;
  options_.mode_height = c.mode_height;
  options_.vrr = c.vrr;
  frame_gen_enabled_ = c.frame_generation;
  last_sync_ = {};  // vsync, VRR or frame generation changed: limit recomputed at the next Present
  if (!mode_changed) return;
  exclusive_failed_ = false;
  if (exclusive_) {
    // It exits here; UpdateFullscreen (below, if exclusive is still wanted) re-enters with the new mode.
    const HRESULT hr = swap_chain_->SetFullscreenState(FALSE, nullptr);
    if (FAILED(hr)) LogHr("SetFullscreenState(FALSE)", hr);
    LogLine("[gpu] exclusive fullscreen left (mode changed by the settings)");
    exclusive_ = false;
    width_ = height_ = 0;
    resize_failed_ = {};
    if (options_.exclusive_wanted) options_.exclusive_wanted(false);
  }
}

void Presenter::PaceFrame() {
  using settings::Vrr;
  if (options_.frame_rate == settings::FrameRate::k30 || !options_.display_sync) return;
  const auto now = std::chrono::steady_clock::now();
  // SyncPoller returns the result of the previous request: with 500 ms a G-Sync change shows up in 0.5-1 s.
  // NVAPI stays on its own thread, here only the last result is read.
  const auto period = options_.vrr == Vrr::kAuto ? std::chrono::milliseconds(500) : std::chrono::milliseconds(2000);
  if (last_sync_ == std::chrono::steady_clock::time_point{} || now - last_sync_ >= period) {
    last_sync_ = now;
    display_ = options_.display_sync(options_.vrr == Vrr::kAuto);
    const uint32_t cost_us =
        uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - now).count());
    // The presenter must not stall for more than 1 ms per query.
    if (cost_us > 1000) LogLine("[frame] monitor query slow: " + std::to_string(cost_us) + " us");
  }
  const plat::DisplaySync& d = display_;
  const bool vrr = frame_pacing::VrrActive(options_.vrr, d.vrr);
  // Real frames at half the cap only while the proxy presents generated frames: in menus, loading and split screen
  // nothing is generated and the limit is the normal one.
  const bool fg = frame_gen_ && frame_gen_enabled_ && frame_gen_state_.Generating();
  limiter_hz_ = fg ? frame_pacing::FrameGenLimiterHz(options_.frame_rate, d.refresh_hz)
                   : frame_pacing::LimiterHz(options_.frame_rate, vrr, options_.vsync, d.refresh_hz);
  const std::tuple state{d.refresh_hz, vrr, limiter_hz_, fg};
  if (state != last_logged_) {
    const std::string reason = options_.vrr == Vrr::kOn    ? "vrr = on"
                               : options_.vrr == Vrr::kOff ? "vrr = off"
                               : d.vrr                     ? "NVAPI enabled=" + std::to_string(int(*d.vrr))
                                                           : d.note;
    char limit[32];
    std::snprintf(limit, sizeof limit, "limit %.2f fps", limiter_hz_);
    LogLine("[frame] monitor " + std::to_string(d.refresh_hz) + " Hz, VRR " + (vrr ? "active" : "not active") +
            " (" + reason + "), " + (limiter_hz_ > 0 ? limit : "no limit") + (fg ? ", frame generation" : "") +
            (last_logged_ ? "" : ", NVAPI in " + std::to_string(d.nvapi_us) + " us"));
    last_logged_ = state;
  }
  limiter_.Wait(limiter_hz_);
}

bool Presenter::Present() {
  if (pending_video_) {
    ApplyVideo(*pending_video_);
    pending_video_.reset();
  }
  if (options_.exclusive_fullscreen) UpdateFullscreen();
  {
    const auto [w, h] = options_.width && options_.height ? std::pair{options_.width, options_.height} : ClientArea();
    if (!w || !h) {  // minimized: nothing to present, frame not counted
      // Skipped frames age the upscaler's history: like a frame without capture (reset, no jitter).
      if (capture_) SetUpscaleCapture(nullptr);
      upscaled_last_ = false;
      // Also for frame generation: the first frame that generates after the restore has reset.
      if (frame_gen_) frame_gen_state_.Next(false, false, std::chrono::steady_clock::now());
      return true;
    }
    if (!ResizeIfNeeded(w, h)) return false;
  }
  // Screenshot due in this frame?
  std::optional<std::filesystem::path> shot;
  {
    std::lock_guard g(screenshot_mutex_);
    if (!screenshots_.empty() && std::chrono::steady_clock::now() >= screenshots_.front().first) {
      shot = std::move(screenshots_.front().second);
      screenshots_.erase(screenshots_.begin());
    }
  }
  if (shot && !EnsureReadback()) shot.reset();
  CollectCaptureShots(false);
  CaptureSlot* cap = nullptr;
  if (pending_capture_) {
    cap = AcquireCaptureSlot();
    if (cap) {
      cap->job = std::move(pending_capture_->first);
      cap->queue = pending_capture_->second;
    } else {
      ++capture_dropped_;
    }
    pending_capture_.reset();
  }

  const uint32_t index =
      (frame_gen_ ? frame_gen_->CurrentBufferIndex() : swap_chain_->GetCurrentBackBufferIndex()) % kBufferCount;
  ID3D12Resource* back = back_buffers_[index].Get();
  // HDR10: the usual SDR image goes into hdr_sdr_, then the PQ composition goes into the real back buffer.
  ID3D12Resource* const real_back = back;
  const bool hdr = HdrActive() && EnsureHdrSdr();
  // Copy: a DisableHdr in mid-frame clears hdr_levels_, this frame still comes out in PQ (SDR fallback).
  const hdr::Levels levels = hdr_levels_.value_or(hdr::Levels{});
  if (hdr) back = hdr_sdr_.Get();
  WaitFor(allocator_fence_[index]);  // the allocator from two rounds ago is free
  HRESULT hr = allocators_[index]->Reset();
  if (SUCCEEDED(hr)) hr = list_->Reset(allocators_[index].Get(), nullptr);
  if (FAILED(hr)) {
    LogHr("Command list reset", hr);
    if (provider_.GetDevice()->GetDeviceRemovedReason() != S_OK) LogDeviceRemoved();
    return false;
  }

  D3D12_RESOURCE_BARRIER b = Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list_->ResourceBarrier(1, &b);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(hdr ? kBufferCount : index);
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr auto kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  // HDR10: residual from the capture, before the Present consumes it (menus, loading, Bink: none).
  ID3D12Resource* residual = nullptr;
  if (hdr && capture_ && capture_->scene && capture_->bloom && guest_output_valid_ && guest_output_ &&
      capture_->prehud->GetDesc().Width == guest_width_ && capture_->prehud->GetDesc().Height == guest_height_) {
    const UpscaleCapture& c = *capture_;
    if (!hdr_residual_ || hdr_residual_->GetDesc().Width != guest_width_ ||
        hdr_residual_->GetDesc().Height != guest_height_) {
      if (hdr_residual_) WaitIdle();  // the previous frame may still be reading it
      hdr_residual_.Reset();
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = guest_width_;
      desc.Height = guest_height_;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      desc.SampleDesc.Count = 1;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(provider_.GetDevice()->CreateCommittedResource(
              &xe::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&hdr_residual_))))
        DisableHdr("residual not created");
    }
    if (hdr_residual_) {
      ID3D12Resource* inputs[] = {c.prehud.Get(), c.depth.Get(), c.velocity.Get(), c.scene.Get(), c.bloom.Get()};
      D3D12_RESOURCE_BARRIER to[6], from[7];
      for (int i = 0; i < 5; ++i) {
        to[i] = Transition(inputs[i], D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        from[i] = Transition(inputs[i], kRead, D3D12_RESOURCE_STATE_COPY_DEST);
      }
      to[5] = Transition(guest_output_.Get(), kGuestOutputInternalState, kRead);
      from[5] = Transition(guest_output_.Get(), kRead, kGuestOutputInternalState);
      from[6] = Transition(hdr_residual_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kPs);
      list_->ResourceBarrier(6, to);
      hdr_passes_->Reconstruct(list_.Get(), c.depth.Get(), c.scene.Get(), c.bloom.Get(), c.velocity.Get(),
                               c.prehud.Get(), guest_output_.Get(), c.hdr, c.ramp,
                               levels.peak_nits / levels.paper_white_nits, hdr_residual_.Get(), index);
      list_->ResourceBarrier(7, from);
      residual = hdr_residual_.Get();  // in PIXEL_SHADER_RESOURCE until the end of the frame
    }
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  list_->ClearRenderTargetView(rtv, black, 0, nullptr);
  D3D12_RESOURCE_STATES back_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

  // Upscaler: reconstructed guest output, if there is a new capture; otherwise the guest output as is.
  const bool upscaled = guest_output_valid_ && guest_output_ && capture_ && DrawUpscaled(rtv, index);
  const std::optional<UpscaleCapture> fg_capture = frame_gen_ ? capture_ : std::nullopt;
  capture_.reset();
  upscaled_last_ = upscaled;
  // Every 10 s with an upscaler: frames it reconstructed out of those presented (the others had no capture).
  if (upscaler_factory_) {
    ++upscale_window_[1];
    if (upscaled) ++upscale_window_[0];
    const auto now = std::chrono::steady_clock::now();
    if (upscale_window_start_ == std::chrono::steady_clock::time_point{}) upscale_window_start_ = now;
    if (now - upscale_window_start_ >= std::chrono::seconds(10)) {
      LogLine("[gpu] upscaler: last 10 s: upscaled " + std::to_string(upscale_window_[0]) + " of " +
              std::to_string(upscale_window_[1]) + " frames");
      upscale_window_[0] = upscale_window_[1] = 0;
      upscale_window_start_ = now;
    }
  }
  // Every 10 s: frames presented, longest gap between two of them (hitches) and the process working set.
  {
    const auto now = std::chrono::steady_clock::now();
    if (perf_window_start_ == std::chrono::steady_clock::time_point{}) perf_window_start_ = perf_last_frame_ = now;
    perf_longest_ = std::max(perf_longest_, now - perf_last_frame_);
    perf_last_frame_ = now;
    ++perf_frames_;
    if (now - perf_window_start_ >= std::chrono::seconds(10)) {
      PROCESS_MEMORY_COUNTERS mem{};
      K32GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem));
      char line[128];
      std::snprintf(line, sizeof(line), "[perf] last 10 s: %llu frames, longest %.1f ms, working set %llu MB",
                    static_cast<unsigned long long>(perf_frames_),
                    std::chrono::duration<double, std::milli>(perf_longest_).count(),
                    static_cast<unsigned long long>(mem.WorkingSetSize >> 20));
      LogLine(line);
      perf_frames_ = 0;
      perf_longest_ = {};
      perf_window_start_ = now;
    }
  }
  // Copy only at 1:1 without sharpening (there ChoosePaintEffect would give kCasSharpen).
  const bool guest = !upscaled && guest_output_valid_ && guest_output_;
  if (guest && (guest_width_ != width_ || guest_height_ != height_ || options_.sharpening)) {
    DrawGuestOutput(rtv, kSrvGuestOutput, guest_width_, guest_height_);
  } else if (guest) {
    D3D12_RESOURCE_BARRIER to_copy[2] = {
        Transition(back, back_state, D3D12_RESOURCE_STATE_COPY_DEST),
        Transition(guest_output_.Get(), kGuestOutputInternalState, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list_->ResourceBarrier(2, to_copy);
    back_state = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_TEXTURE_COPY_LOCATION dst{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{guest_output_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = Transition(guest_output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kGuestOutputInternalState);
    list_->ResourceBarrier(1, &b);
  }

  // Frame generation: after the game image (with the HUD), before the overlay (outside the HUD-less); with HDR
  // after the composition. Overlay open: no inputs (DLSS would lose it in generated frames, drawn after).
  const auto frame_generation = [&] {
    const bool has_inputs = fg_capture && guest_output_valid_ && guest_output_ && upscale_passes_ &&
                            fg_capture->prehud->GetDesc().Width == guest_width_ &&
                            fg_capture->prehud->GetDesc().Height == guest_height_ && !(overlay_ && overlay_->IsOpen());
    const FrameGenFrame f = frame_gen_state_.Next(frame_gen_enabled_, has_inputs, std::chrono::steady_clock::now());
    if (!f.generate) return frame_gen_->Frame(swap_chain_.Get(), width_, height_, f, nullptr);
    if (back_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {  // Frame's contract (1:1 copy of the guest output)
      b = Transition(back, back_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list_->ResourceBarrier(1, &b);
      back_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    FrameGenWithInputs(*fg_capture, index, upscaled, f, residual);
  };
  if (frame_gen_ && !hdr) frame_generation();

  // HDR10 with the overlay open: copy of the SDR without overlay (the composition removes the residual there).
  bool pre_overlay = false;
  if (hdr && overlay_ && overlay_->Visible()) {
    if (!hdr_pre_overlay_) {
      const D3D12_RESOURCE_DESC desc = hdr_sdr_->GetDesc();
      if (FAILED(provider_.GetDevice()->CreateCommittedResource(
              &xe::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&hdr_pre_overlay_))))
        DisableHdr("copy before the overlay not created");
    }
    if (hdr_pre_overlay_) {
      b = Transition(back, back_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list_->ResourceBarrier(1, &b);
      back_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      list_->CopyResource(hdr_pre_overlay_.Get(), back);
      pre_overlay = true;
    }
  }

  if (overlay_ && overlay_->Visible()) {
    if (back_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
      b = Transition(back, back_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list_->ResourceBarrier(1, &b);
      back_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    overlay_->Draw(provider_.GetDevice(), provider_.GetDirectQueue(), list_.Get(), rtv, width_, height_);
  }

  const auto copy_to_readback = [&](ID3D12Resource* src, ID3D12Resource* readback,
                                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprint = nullptr) {
    D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint ? *footprint : readback_footprint_;
    D3D12_TEXTURE_COPY_LOCATION from{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    from.SubresourceIndex = 0;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
  };
  // HDR10: PQ of hdr_sdr_ (+ residual) into the real back buffer; the BMP screenshot is the SDR of hdr_sdr_.
  if (hdr) {
    D3D12_RESOURCE_BARRIER to[3] = {
        Transition(back, back_state, kPs),
        Transition(real_back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(hdr_pre_overlay_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kPs)};
    list_->ResourceBarrier(pre_overlay ? 3 : 2, to);
    const Rect fr = FitRect(guest_width_, guest_height_, width_, height_, options_.stretch);
    const D3D12_RECT rect{LONG(fr.x), LONG(fr.y), LONG(fr.x + fr.w), LONG(fr.y + fr.h)};
    hdr_passes_->Compose(list_.Get(), back, pre_overlay ? hdr_pre_overlay_.Get() : nullptr, residual, rect,
                         levels.paper_white_nits, Rtv(index), width_, height_, index, 0);
    if (pre_overlay) {
      b = Transition(hdr_pre_overlay_.Get(), kPs, D3D12_RESOURCE_STATE_COPY_DEST);
      list_->ResourceBarrier(1, &b);
    }
    back_state = kPs;
    if (shot || cap) {
      b = Transition(back, back_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list_->ResourceBarrier(1, &b);
      back_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      if (shot) copy_to_readback(back, readback_.Get());
      if (cap) copy_to_readback(back, cap->buffer.Get(), &cap->footprint);
    }
    b = Transition(back, back_state, D3D12_RESOURCE_STATE_COMMON);
    list_->ResourceBarrier(1, &b);
    back = real_back;
    back_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (frame_gen_) frame_generation();
    if (residual) {
      b = Transition(residual, kPs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list_->ResourceBarrier(1, &b);
    }
  }

  if (shot || (cap && !hdr)) {
    b = Transition(back, back_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &b);
    back_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (shot) copy_to_readback(back, hdr ? readback_pq_.Get() : readback_.Get());
    if (cap && !hdr) copy_to_readback(back, cap->buffer.Get(), &cap->footprint);
  }

  b = Transition(back, back_state, D3D12_RESOURCE_STATE_PRESENT);
  list_->ResourceBarrier(1, &b);
  if (FAILED(hr = list_->Close())) {
    LogHr("Command list close", hr);
    return false;
  }
  ID3D12CommandList* lists[] = {list_.Get()};
  provider_.GetDirectQueue()->ExecuteCommandLists(1, lists);
  allocator_fence_[index] = Signal();
  if (cap) cap->fence = allocator_fence_[index];
  if (wait_gpu_before_present_) WaitFor(allocator_fence_[index]);

  // Tearing only without vsync and if supported (swap chain flag); never in exclusive.
  const bool tearing = (swap_chain_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) && !options_.vsync && !exclusive_;
  PaceFrame();
  const UINT sync = options_.vsync ? 1 : 0, present_flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
  hr = frame_gen_ ? frame_gen_->Present(sync, present_flags) : swap_chain_->Present(sync, present_flags);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
      provider_.GetDevice()->GetDeviceRemovedReason() != S_OK) {
    LogDeviceRemoved();
    return false;
  }
  if (FAILED(hr)) {
    LogHr("Present", hr);
    return false;
  }
  const uint64_t frame = frames_presented_.fetch_add(1) + 1;
  kernel::stats::NoteHostFramePresented();
  if (shot) {
    WaitFor(allocator_fence_[index]);
    SaveScreenshot(*shot, frame, hdr);
  }
  return true;
}

void Presenter::FrameGenWithInputs(const UpscaleCapture& c, uint32_t slot, bool upscaled, const FrameGenFrame& f,
                                   ID3D12Resource* hdr_residual) {
  const D3D12_RESOURCE_DESC pd = c.prehud->GetDesc();
  const uint32_t rw = uint32_t(pd.Width), rh = pd.Height;
  // Without inputs this frame does not generate, but Frame is called anyway once per Present.
  const auto without_inputs = [&] {
    frame_gen_->Frame(swap_chain_.Get(), width_, height_, {f.frame_id, false, false, f.frame_ms}, nullptr);
  };
  // motion_ and reactive_ are needed even without an upscaler (minimal upscaled_, unused).
  if (!upscaled && !EnsureUpscaleTargets(rw, rh, 8, 8)) return without_inputs();
  ComPtr<ID3D12Resource>& hl = hudless_[slot];
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (!hl || hl->GetDesc().Width != width_ || hl->GetDesc().Height != height_) {
    if (hl) WaitIdle(), frame_gen_->WaitForPresents();  // the proxy reads the HUD-less at Present
    hl.Reset();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width_;
    desc.Height = height_;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(provider_.GetDevice()->CreateCommittedResource(&xe::ui::d3d12::util::kHeapPropertiesDefault,
                                                              D3D12_HEAP_FLAG_NONE, &desc, kRead, nullptr,
                                                              IID_PPV_ARGS(&hl)))) {
      LogLine("[fg] HUD-less not created");
      return without_inputs();
    }
  }
  // With HDR the generator receives the HUD-less in PQ (same domain as the back buffer), R10G10B10A2 like the RTV
  // of the composition; it follows the back buffer (zeroed in ResizeIfNeeded).
  ComPtr<ID3D12Resource>& pq = hdr_hudless_[slot];
  if (HdrActive() && !pq) {
    D3D12_RESOURCE_DESC desc = hl->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(provider_.GetDevice()->CreateCommittedResource(&xe::ui::d3d12::util::kHeapPropertiesDefault,
                                                              D3D12_HEAP_FLAG_NONE, &desc, kRead, nullptr,
                                                              IID_PPV_ARGS(&pq)))) {
      DisableHdr("PQ HUD-less not created");
      return without_inputs();
    }
    provider_.GetDevice()->CreateRenderTargetView(pq.Get(), nullptr, Rtv(kBufferCount + 1 + slot));
  }
  ID3D12Resource* base = upscaled ? upscaled_.Get() : guest_output_.Get();
  ID3D12Resource* inputs[] = {c.prehud.Get(), c.depth.Get(), c.velocity.Get()};
  std::vector<D3D12_RESOURCE_BARRIER> to, back;
  for (ID3D12Resource* r : inputs) {
    to.push_back(Transition(r, D3D12_RESOURCE_STATE_COPY_DEST, kRead));
    back.push_back(Transition(r, kRead, D3D12_RESOURCE_STATE_COPY_DEST));
  }
  to.push_back(Transition(guest_output_.Get(), kGuestOutputInternalState, kRead));
  back.push_back(Transition(guest_output_.Get(), kRead, kGuestOutputInternalState));
  if (upscaled) {
    to.push_back(Transition(upscaled_.Get(), kUav, kRead));
    back.push_back(Transition(upscaled_.Get(), kRead, kUav));
  }
  to.push_back(Transition(hl.Get(), kRead, kUav));
  list_->ResourceBarrier(UINT(to.size()), to.data());
  // With the upscaler DrawUpscaled has already written them from this capture, same slot and same ramp (then read by
  // Hudless); they stay in UAV and the mid barrier brings them to read.
  if (!upscaled) {
    upscale_passes_->MotionVectors(list_.Get(), c.depth.Get(), c.velocity.Get(), motion_.Get(), c.k, slot);
    upscale_passes_->ReactiveMask(list_.Get(), c.prehud.Get(), guest_output_.Get(), c.ramp, reactive_.Get(), slot);
  }
  D3D12_RESOURCE_BARRIER mid[2] = {Transition(motion_.Get(), kUav, kRead), Transition(reactive_.Get(), kUav, kRead)};
  list_->ResourceBarrier(2, mid);
  // Rectangle in which DrawUpscaled / DrawGuestOutput drew base (black bands outside).
  const D3D12_RESOURCE_DESC bd = base->GetDesc();
  const Rect r = FitRect(uint32_t(bd.Width), bd.Height, width_, height_, options_.stretch);
  const D3D12_RECT rect{LONG(r.x), LONG(r.y), LONG(r.x + r.w), LONG(r.y + r.h)};
  // Same slot and same pwl as the ReactiveMask just recorded: Hudless reads its ramp.
  upscale_passes_->Hudless(list_.Get(), base, c.prehud.Get(), reactive_.Get(), c.ramp.pwl, rect, hl.Get(), slot);
  const D3D12_RESOURCE_BARRIER hl_done = Transition(hl.Get(), kUav, kRead);
  list_->ResourceBarrier(1, &hl_done);

  FrameGenInputs in{list_.Get(), c.depth.Get(), motion_.Get(), hl.Get(), rw, rh, c.jitter_x, c.jitter_y, 0.0f, &c.k};
  if (HdrActive()) {
    D3D12_RESOURCE_BARRIER to_rt = Transition(pq.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &to_rt);
    hdr_passes_->Compose(list_.Get(), hl.Get(), nullptr, hdr_residual, rect, hdr_levels_->paper_white_nits,
                         Rtv(kBufferCount + 1 + slot), width_, height_, slot, 1);
    to_rt = Transition(pq.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    list_->ResourceBarrier(1, &to_rt);
    in.hudless = pq.Get();
    in.hdr_peak_nits = hdr_levels_->peak_nits;
  }
  frame_gen_->Frame(swap_chain_.Get(), width_, height_, f, &in);

  back.push_back(Transition(motion_.Get(), kRead, kUav));
  back.push_back(Transition(reactive_.Get(), kRead, kUav));
  list_->ResourceBarrier(UINT(back.size()), back.data());
}

void Presenter::QueueCaptureShot(ImageJob job, ImageWriteQueue* queue) {
  if (pending_capture_) ++capture_dropped_;
  pending_capture_.emplace(std::move(job), queue);
}

void Presenter::FlushCaptureShots() { CollectCaptureShots(true); }

Presenter::CaptureSlot* Presenter::AcquireCaptureSlot() {
  CaptureSlot* slot = nullptr;
  for (CaptureSlot& s : capture_slots_)
    if (!s.fence) {
      slot = &s;
      break;
    }
  if (!slot) return nullptr;
  const D3D12_RESOURCE_DESC desc = back_buffers_[0]->GetDesc();
  UINT64 total = 0;
  provider_.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &slot->footprint, nullptr, nullptr, &total);
  if (slot->buffer && slot->size == total) return slot;
  // Free slot: its last copy has completed, the buffer can be replaced.
  slot->buffer.Reset();
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const HRESULT hr = provider_.GetDevice()->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot->buffer));
  if (FAILED(hr)) {
    LogHr("CreateCommittedResource (capture readback)", hr);
    slot->size = 0;
    return nullptr;
  }
  slot->size = total;
  return slot;
}

void Presenter::CollectCaptureShots(bool wait) {
  for (CaptureSlot& s : capture_slots_) {
    if (!s.fence) continue;
    if (wait) WaitFor(s.fence);
    if (fence_->GetCompletedValue() < s.fence) continue;
    const D3D12_SUBRESOURCE_FOOTPRINT& fp = s.footprint.Footprint;
    uint8_t* mapped = nullptr;
    const HRESULT hr = s.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (SUCCEEDED(hr)) {
      s.job.width = fp.Width;
      s.job.height = fp.Height;
      s.job.pixels.resize(size_t(fp.Width) * fp.Height);
      for (uint32_t y = 0; y < fp.Height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(mapped + s.footprint.Offset + size_t(y) * fp.RowPitch);
        for (uint32_t x = 0; x < fp.Width; ++x) s.job.pixels[size_t(y) * fp.Width + x] = ToXrgb8888(row[x]);
      }
      const D3D12_RANGE written{0, 0};
      s.buffer->Unmap(0, &written);
      if (s.queue) s.queue->Push(std::move(s.job));
    } else {
      LogHr("Capture readback map", hr);
      ++capture_dropped_;
    }
    s.job = {};
    s.queue = nullptr;
    s.fence = 0;
  }
}

void Presenter::SaveScreenshot(const std::filesystem::path& file, uint64_t frame, bool pq) {
  const D3D12_SUBRESOURCE_FOOTPRINT& fp = readback_footprint_.Footprint;
  // BMP from the SDR (readback_); with pq also <file without extension>.pq.ppm from the PQ back buffer (readback_pq_).
  for (const bool ppm : {false, true}) {
    if (ppm && !pq) break;
    ID3D12Resource* readback = ppm ? readback_pq_.Get() : readback_.Get();
    const std::filesystem::path out = ppm ? std::filesystem::path(file).replace_extension(".pq.ppm") : file;
    // The whole buffer: the last row has no RowPitch padding, so
    // RowPitch * Height can exceed its size (widths not multiple of 64).
    uint8_t* mapped = nullptr;
    const HRESULT hr = readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
      LogHr("Readback map", hr);
      return;
    }
    std::vector<uint32_t> pixels(size_t(fp.Width) * fp.Height);
    for (uint32_t y = 0; y < fp.Height; ++y) {
      const auto* row =
          reinterpret_cast<const uint32_t*>(mapped + readback_footprint_.Offset + size_t(y) * fp.RowPitch);
      for (uint32_t x = 0; x < fp.Width; ++x) pixels[size_t(y) * fp.Width + x] = ppm ? row[x] : ToXrgb8888(row[x]);
    }
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    if (!(ppm ? WritePpm10(out, fp.Width, fp.Height, pixels) : WriteBmp24(out, fp.Width, fp.Height, pixels))) {
      LogLine("[gpu] screenshot not written: " + Show(out));
      return;
    }
    LogLine("[gpu] screenshot " + Show(out) + " " + std::to_string(fp.Width) + "x" + std::to_string(fp.Height) +
            " frame host " + std::to_string(frame));
  }
}

}  // namespace gpu
