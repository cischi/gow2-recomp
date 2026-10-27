// See frame_generation.h. DLSS frame generation (NGX feature 11) through the MIT bridge gow2_dlss.dll
// (dlss_bridge/gow2_dlss_bridge.h, ABI 3).
//
// The presenter draws into a ring of generator textures (one per presenter buffer). Frame records
// the evaluate in the frame's command list: there the image is already finished (HUD included; the overlay, drawn after,
// turns generation off), so depth, motion vectors and HUD-less are valid without copies. Present queues the
// frame to the timing thread, which waits for the frame's fence, copies interpolated and real into the real swap
// chain's back buffers and presents at the FramePacer instants. Copies and Present live on their own high-priority queue (the
// real swap chain's): on the game queue they waited for the next frame's work. The presenter does not take back a
// ring buffer until the timing thread has recorded its copies (CurrentBufferIndex waits) and the game
// queue waits on the GPU for them to finish.
#include "gpu/frame_generation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/dlss_bridge_loader.h"
#include "gpu/dlss_fg_math.h"
#include "gpu/frame_pacer.h"
#include "log.h"
#include "platform/platform.h"

namespace gpu {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kSwapChainBuffers = 3;
constexpr UINT kCopyAllocators = 4;  // timing thread copies in flight on the GPU (2 per frame)
// shortcut: approximate camera like FSR 3 FG (near 10, far 1e6, vertical FOV 1 rad), the real camera from the
// game's constants if the visual check shows defects in rotation or in disocclusions.
constexpr float kNear = 10.0f, kFar = 1e6f, kFovY = 1.0f;

void Unavailable(const std::string& reason) {
  LogLine("[fg] DLSS frame generation not available (" + reason + "): normal swap chain");
}

void OnBridgeLog(const char* line) { LogLine(std::string("[fg] DLSS: ") + (line ? line : "")); }

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER x{};
  x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  x.Transition.pResource = r;
  x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  x.Transition.StateBefore = a;
  x.Transition.StateAfter = b;
  cmd->ResourceBarrier(1, &x);
}

void WaitFence(ID3D12Fence* fence, uint64_t value) {
  if (fence->GetCompletedValue() < value) fence->SetEventOnCompletion(value, nullptr);  // blocks
}

void WaitUntil(PacerClock::time_point at) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(at - PacerClock::now()).count();
  if (us > 1500) plat::SleepPrecise(uint32_t(us - 1000));
  while (PacerClock::now() < at) {
  }
}

void Copy(float (&to)[16], const Mat4& m) { std::copy(m.begin(), m.end(), to); }

class DlssFrameGenerator final : public FrameGenerator {
 public:
  DlssFrameGenerator(const DlssBridge& b, std::filesystem::path dir, const SwapChainRequest& req,
                     IDXGISwapChain4* swap_chain, ComPtr<ID3D12CommandQueue> present_queue)
      : bridge_(b), dir_(std::move(dir)), device_(req.device), queue_(req.queue),
        present_queue_(std::move(present_queue)), sc_(swap_chain), ring_(std::max(req.desc.BufferCount, 1u)) {}

  ~DlssFrameGenerator() override {
    StopThread();
    if (ctx_) bridge_.api->FgDestroy(ctx_);
    FreeLibrary(bridge_.dll);
  }

  // Reason for the failure in the log.
  bool Init() {
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_fence_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&create_alloc_))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, create_alloc_.Get(), nullptr,
                                          IID_PPV_ARGS(&create_list_))))
      return Unavailable("fence or command list not created"), false;
    create_list_->Close();
    for (auto& a : copy_allocs_)
      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
        return Unavailable("command allocator not created"), false;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_allocs_[0].Get(), nullptr,
                                          IID_PPV_ARGS(&copy_list_))))
      return Unavailable("command list not created"), false;
    copy_list_->Close();
    if (!EnsureRing()) return Unavailable("texture ring not created"), false;
    if (!EnsureFeature(width_, height_, width_, height_, false)) return Unavailable("feature not created"), false;
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  HRESULT GetBuffer(UINT i, ID3D12Resource** out) override {
    if (!EnsureRing()) return E_OUTOFMEMORY;
    if (i >= ring_.size()) return E_INVALIDARG;
    return ring_[i].real.CopyTo(out);
  }

  UINT CurrentBufferIndex() override {
    // This frame's buffer is the one from ring_.size() frames ago: free when fewer remain in the queue.
    std::unique_lock l(mutex_);
    cv_.wait(l, [&] { return in_flight_ < ring_.size(); });
    const UINT i = UINT(presents_ % ring_.size());
    // The buffer's copies are on the present queue: the frame's work does not rewrite it before they finish.
    if (const uint64_t copied = ring_[i].copied) queue_->Wait(copy_fence_.Get(), copied);
    return i;
  }

  void Frame(IDXGISwapChain4*, uint32_t w, uint32_t h, const FrameGenFrame& f, const FrameGenInputs* in) override {
    pending_generated_ = pending_reset_ = false;
    bool generated = false;
    if (in && !eval_broken_ && ring_[0].real && w == width_ && h == height_ &&
        EnsureFeature(w, h, in->render_w, in->render_h, in->hdr_peak_nits > 0))
      generated = Evaluate(*in, f.reset);
    if (generated != enabled_) {
      LogLine(std::string("[fg] generation ") + (generated ? "on" : "off") + " at frame " +
              std::to_string(f.frame_id));
      enabled_ = generated;
    }
  }

  HRESULT Present(UINT sync_interval, UINT flags) override {
    if (const HRESULT hr = present_hr_.load(); FAILED(hr)) return hr;
    queue_->Signal(frame_fence_.Get(), ++frame_value_);
    {
      std::lock_guard g(mutex_);
      queue_items_.push_back({UINT(presents_ % ring_.size()), pending_generated_, pending_reset_, sync_interval,
                              flags, frame_value_});
      ++in_flight_;
      ++presents_;
    }
    cv_.notify_all();
    pending_generated_ = pending_reset_ = false;
    return S_OK;
  }

  void WaitForPresents() override {
    uint64_t copied = 0;
    {
      std::unique_lock l(mutex_);
      cv_.wait(l, [&] { return in_flight_ == 0; });
      copied = ++copy_value_;  // timing thread stopped with an empty queue
    }
    // Copies, Present and evaluate already queued finished on the GPU, on both queues: no use of the back buffers
    // of the real swap chain (with the debug layer, releasing it with the present queue not drained crashes the process).
    present_queue_->Signal(copy_fence_.Get(), copied);
    queue_->Signal(frame_fence_.Get(), ++frame_value_);
    WaitFence(frame_fence_.Get(), frame_value_);
    WaitFence(copy_fence_.Get(), copied);
  }

  void Shutdown(ComPtr<IDXGISwapChain4>& swap_chain) override {
    StopThread();
    if (ctx_) bridge_.api->FgDestroy(ctx_);
    ctx_ = nullptr;
    ring_.clear();
    sc_ = nullptr;
    swap_chain.Reset();
    LogLine("[fg] closing: real " + std::to_string(real_.load()) + ", generated " + std::to_string(generated_.load()));
  }

 private:
  struct Slot {
    ComPtr<ID3D12Resource> real;    // where the presenter draws; at rest COMMON (= PRESENT)
    ComPtr<ID3D12Resource> interp;  // evaluate output; at rest UNORDERED_ACCESS
    uint64_t copied = 0;            // copy_fence_ after the last copies from the buffer (under mutex_)
  };
  struct Item {
    UINT slot;
    bool generated, reset;
    UINT sync_interval, flags;
    uint64_t fence;  // frame_fence_ after the frame's work
  };

  // Ring with the current dimensions and format of the real swap chain; recreated (after WaitForPresents) if they change.
  bool EnsureRing() {
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (!sc_ || FAILED(sc_->GetDesc1(&d))) return false;
    if (ring_[0].real && d.Width == width_ && d.Height == height_ && d.Format == format_) return true;
    if (ring_[0].real) WaitForPresents();
    width_ = d.Width, height_ = d.Height, format_ = d.Format;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = d.Width;
    rd.Height = d.Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = d.Format;
    rd.SampleDesc.Count = 1;
    for (Slot& s : ring_) {
      s = {};
      rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_RESOURCE_DESC ud = rd;
      ud.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&s.real))) ||
          FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&s.interp)))) {
        LogLine("[fg] DLSS ring textures not created (" + std::to_string(d.Width) + "x" +
                std::to_string(d.Height) + ")");
        for (Slot& x : ring_) x = {};
        return false;
      }
    }
    return true;
  }

  // Feature for output w x h, render up to rw x rh and HDR or SDR; recreated (GPU idle) if they change. After a
  // failed creation no new attempt with the same configuration.
  bool EnsureFeature(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, bool hdr) {
    const Gow2DlssFgConfig& m = made_;
    const bool same = w == m.width && h == m.height && rw <= m.render_w && rh <= m.render_h && hdr == (m.hdr != 0);
    if (same) return ctx_ != nullptr;
    if (ctx_) {
      WaitForPresents();
      bridge_.api->FgDestroy(ctx_);
      ctx_ = nullptr;
    }
    eval_broken_ = false;
    made_ = {dir_.c_str(), w, h, std::max(rw, w), std::max(rh, h), format_, hdr ? 1 : 0};
    create_alloc_->Reset();
    create_list_->Reset(create_alloc_.Get(), nullptr);
    ctx_ = bridge_.api->FgCreate(device_, create_list_.Get(), &made_, OnBridgeLog);
    create_list_->Close();
    ID3D12CommandList* lists[] = {create_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(frame_fence_.Get(), ++frame_value_);
    WaitFence(frame_fence_.Get(), frame_value_);
    if (!ctx_) {
      if (thread_.joinable())  // at startup the line is Unavailable's
        LogLine("[fg] DLSS FG feature not created (" + std::to_string(w) + " x " + std::to_string(h) +
              "): no frame generated");
      return false;
    }
    recreated_ = true;
    const char* desc = bridge_.api->FgDescription(ctx_);
    LogLine(std::string("[fg] ") + (desc && *desc ? desc : "DLSS frame generation") + " (" + std::to_string(w) +
            " x " + std::to_string(h) + ")" + (hdr ? ", HDR" : ""));
    return true;
  }

  // Evaluate in the frame's command list; true if the frame has its interpolated one.
  bool Evaluate(const FrameGenInputs& in, bool reset) {
    reset = reset || recreated_;
    recreated_ = false;
    Gow2DlssFgFrame d{};
    const Slot& s = ring_[presents_ % ring_.size()];
    d.cmd = in.cmd;
    d.backbuffer = s.real.Get();
    d.depth = in.depth;
    d.motion = in.motion;
    d.hudless = in.hudless;
    d.output = s.interp.Get();
    d.render_w = in.render_w;
    d.render_h = in.render_h;
    d.jitter_x = in.jitter_x;
    d.jitter_y = in.jitter_y;
    d.mv_scale_x = d.mv_scale_y = 1.0f;  // the presenter's motion_ already in render pixels
    d.near_z = kNear;
    d.far_z = kFar;
    d.fov_y = kFovY;
    d.aspect = float(in.render_w) / float(std::max(in.render_h, 1u));
    d.depth_inverted = 1;
    const Mat4 view_to_clip = ViewToClip(kFovY, d.aspect, kNear, kFar);
    const std::optional<Mat4> clip_to_view = Invert4x4(view_to_clip);
    const Mat4 to_prev = in.k ? ClipToPrevClip(*in.k) : Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const std::optional<Mat4> from_prev = Invert4x4(to_prev);
    reset = reset || !in.k || !from_prev || !clip_to_view;  // unusable matrices: no history
    Copy(d.clip_to_prev_clip, to_prev);
    Copy(d.prev_clip_to_clip, from_prev.value_or(to_prev));
    Copy(d.view_to_clip, view_to_clip);
    Copy(d.clip_to_view, clip_to_view.value_or(view_to_clip));
    d.reset = reset ? 1 : 0;
    Barrier(in.cmd, d.backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const int32_t rc = bridge_.api->FgEvaluate(ctx_, &d);
    Barrier(in.cmd, d.backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (rc != 0) {  // reason in the log from the bridge; from here only real frames until the feature is recreated
      eval_broken_ = true;
      LogLine("[fg] DLSS FG evaluate failed (code " + std::to_string(rc) + "): no frame generated");
      return false;
    }
    // On reset the interpolated is a copy of the real (FG guide): only the real, as in frames without generation.
    pending_generated_ = !reset;
    pending_reset_ = reset;
    return true;
  }

  void StopThread() {
    if (!thread_.joinable()) return;
    WaitForPresents();
    {
      std::lock_guard g(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  // ---- timing thread ----

  void Run() {
    for (;;) {
      Item it;
      {
        std::unique_lock l(mutex_);
        cv_.wait(l, [&] { return stop_ || !queue_items_.empty(); });
        if (queue_items_.empty()) return;
        it = queue_items_.front();
        queue_items_.pop_front();
      }
      WaitFence(frame_fence_.Get(), it.fence);
      present_queue_->Wait(frame_fence_.Get(), it.fence);  // already passed: ordering between queues for the GPU too
      for (const PacedPresent& p : pacer_.Schedule(it.generated, it.reset, PacerClock::now())) {
        WaitUntil(p.at);
        Show(it, p.generated);
      }
      {
        std::lock_guard g(mutex_);
        ring_[it.slot].copied = copy_value_;
        --in_flight_;
      }
      cv_.notify_all();
    }
  }

  // Copy into the real swap chain's current back buffer and Present.
  void Show(const Item& it, bool generated) {
    const Slot& s = ring_[it.slot];
    ID3D12Resource* src = generated ? s.interp.Get() : s.real.Get();
    const D3D12_RESOURCE_STATES rest =
        generated ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON;
    ID3D12CommandAllocator* alloc = copy_allocs_[copies_ % kCopyAllocators].Get();
    uint64_t& alloc_value = copy_alloc_values_[copies_ % kCopyAllocators];
    ++copies_;
    WaitFence(copy_fence_.Get(), alloc_value);
    ComPtr<ID3D12Resource> dst;
    HRESULT hr = sc_->GetBuffer(sc_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&dst));
    if (SUCCEEDED(hr)) hr = alloc->Reset();
    if (SUCCEEDED(hr)) hr = copy_list_->Reset(alloc, nullptr);
    if (FAILED(hr)) return Fail(hr);
    Barrier(copy_list_.Get(), src, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(copy_list_.Get(), dst.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_list_->CopyResource(dst.Get(), src);
    Barrier(copy_list_.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
    Barrier(copy_list_.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(hr = copy_list_->Close())) return Fail(hr);
    ID3D12CommandList* lists[] = {copy_list_.Get()};
    present_queue_->ExecuteCommandLists(1, lists);
    present_queue_->Signal(copy_fence_.Get(), ++copy_value_);
    alloc_value = copy_value_;
    dst.Reset();
    SampleDisplayed();
    if (FAILED(hr = sc_->Present(it.sync_interval, it.flags))) return Fail(hr);
    Count(generated);
  }

  void Fail(HRESULT hr) {
    HRESULT ok = S_OK;
    if (present_hr_.compare_exchange_strong(ok, hr)) {
      char line[96];
      std::snprintf(line, sizeof line, "[fg] DLSS swap chain Present failed (0x%08X)", unsigned(hr));
      LogLine(line);
    }
  }

  // Label of the Present just made, counts and line every 10 s (like FSR 3 FG).
  void Count(bool generated) {
    UINT last = 0;
    if (SUCCEEDED(sc_->GetLastPresentCount(&last))) labels_[last % labels_.size()] = {last, generated};
    SampleDisplayed();
    (generated ? generated_ : real_).fetch_add(1);
    ++(generated ? window_generated_ : window_real_);
    const auto now = PacerClock::now();
    if (window_start_ == PacerClock::time_point{}) window_start_ = now;
    if (now - window_start_ < std::chrono::seconds(10)) return;
    std::string line =
        "[fg] last 10 s: real " + std::to_string(window_real_) + ", generated " + std::to_string(window_generated_);
    if (const std::optional<float> p = MedianHalfIntervalPercent(displayed_)) {  // omitted without statistics
      char tail[64];
      std::snprintf(tail, sizeof tail, ", half-interval median %.1f%%", *p);
      line += tail;
    }
    displayed_.clear();
    LogLine(line);
    window_start_ = now;
    window_real_ = window_generated_ = 0;
  }

  // Last frame on screen, if new and labeled (before and after each Present: one new count per Present).
  void SampleDisplayed() {
    DXGI_FRAME_STATISTICS s{};
    if (FAILED(sc_->GetFrameStatistics(&s)) || s.PresentCount == last_count_) return;
    last_count_ = s.PresentCount;
    const CountLabel& l = labels_[s.PresentCount % labels_.size()];
    if (l.count == s.PresentCount) displayed_.push_back({l.generated, s.PresentCount, s.SyncQPCTime.QuadPart});
  }

  DlssBridge bridge_;
  std::filesystem::path dir_;  // the feature config holds its c_str()
  ID3D12Device* device_;
  ID3D12CommandQueue* queue_;  // the game's: evaluate and frame work
  // Of the real swap chain, high priority: copies and Present do not wait for the next frame's work.
  ComPtr<ID3D12CommandQueue> present_queue_;
  IDXGISwapChain4* sc_;  // the presenter's until Shutdown

  // Ring: written by the Present thread only with an empty queue (EnsureRing after WaitForPresents), read by both.
  // shortcut: as many buffers as the presenter (kBufferCount = 2, not the 3 first planned): while the presenter draws
  // a frame the timing thread holds at most one; raise kBufferCount to 3 if the pace drops when the GPU is saturated.
  std::vector<Slot> ring_;
  UINT width_ = 0, height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

  // Present thread.
  void* ctx_ = nullptr;
  Gow2DlssFgConfig made_{};
  bool recreated_ = false, enabled_ = false;
  bool eval_broken_ = false;  // evaluate failed: no more calls until the feature is recreated
  bool pending_generated_ = false, pending_reset_ = false;  // from Frame to the Present of the same frame
  ComPtr<ID3D12Fence> frame_fence_;
  uint64_t frame_value_ = 0;
  ComPtr<ID3D12CommandAllocator> create_alloc_;
  ComPtr<ID3D12GraphicsCommandList> create_list_;

  // Shared.
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_items_;
  size_t in_flight_ = 0;  // queued and not yet fully presented
  uint64_t presents_ = 0;
  bool stop_ = false;
  std::thread thread_;
  std::atomic<HRESULT> present_hr_{S_OK};
  std::atomic<uint64_t> real_{0}, generated_{0};

  // Timing thread.
  FramePacer pacer_;
  ComPtr<ID3D12Fence> copy_fence_;
  uint64_t copy_value_ = 0, copies_ = 0;
  std::array<ComPtr<ID3D12CommandAllocator>, kCopyAllocators> copy_allocs_;
  std::array<uint64_t, kCopyAllocators> copy_alloc_values_{};
  ComPtr<ID3D12GraphicsCommandList> copy_list_;
  uint64_t window_real_ = 0, window_generated_ = 0;
  PacerClock::time_point window_start_{};
  struct CountLabel {
    uint32_t count;  // PresentCount of the real swap chain
    bool generated;
  };
  std::array<CountLabel, 64> labels_{};  // indexed by PresentCount % 64: more than the frames queued between Present and scanout
  uint32_t last_count_ = 0;
  std::vector<DisplayedFrame> displayed_;  // of the 10 s window
};

}  // namespace

std::unique_ptr<FrameGenerator> CreateDlssFrameGenerator(const SwapChainRequest& req, ComPtr<IDXGISwapChain4>& out,
                                                         const std::filesystem::path& dll_dir) {
  out.Reset();
  std::string why;
  const std::optional<DlssBridge> b = LoadDlssBridge(dll_dir, why);
  if (!b) return Unavailable(why), nullptr;
  const auto give_up = [&](const std::string& reason) {
    Unavailable(reason);
    out.Reset();
    FreeLibrary(b->dll);
    return nullptr;
  };
  if (b->api->abi < 3) return give_up("ABI " + std::to_string(b->api->abi) + " bridge, 3 required");
  Gow2DlssFgCaps caps{};
  if (b->api->FgQuery(req.device, dll_dir.c_str(), &caps, OnBridgeLog) != 0)
    return give_up("NGX capabilities not read");
  if (!caps.available)
    return give_up(caps.needs_driver ? "driver " + std::to_string(caps.min_driver_major) + "." +
                                           std::to_string(caps.min_driver_minor) + " or later required"
                                     : "GPU or driver without DLSS frame generation");
  DXGI_SWAP_CHAIN_DESC1 desc = req.desc;
  desc.BufferCount = kSwapChainBuffers;
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  ComPtr<ID3D12CommandQueue> present_queue;
  ComPtr<IDXGISwapChain1> sc;
  HRESULT hr = req.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&present_queue));
  if (SUCCEEDED(hr))
    hr = req.factory->CreateSwapChainForHwnd(present_queue.Get(), req.hwnd, &desc, nullptr, nullptr, &sc);
  if (SUCCEEDED(hr)) hr = sc.As(&out);
  if (FAILED(hr)) {
    char reason[64];
    std::snprintf(reason, sizeof reason, "swap chain not created, 0x%08X", unsigned(hr));
    return give_up(reason);
  }
  auto g = std::make_unique<DlssFrameGenerator>(*b, dll_dir, req, out.Get(), present_queue);  // owns the DLL from here
  if (!g->Init()) {
    g.reset();
    out.Reset();
    return nullptr;
  }
  return g;
}

}  // namespace gpu
