// Shader capture (--shader-capture, F11 in game): state of a recording without D3D12. While active, every draw is
// noted (hashes and modifications of its VS/PS translations, from Xenia's patch 0021 hook), each (hash,
// modification) is reported as new once so the backend dumps it to <dir>/shaders, and a shot is due every 200 ms
// (frames/<index>.png + .txt with FormatFrameList). The D3D12 side is in d3d12_backend.cpp and presenter.cpp.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gpu {

// <dir>/shader_captures/<game_id>/YYYY-MM-DD_HH-MM-SS (local time t).
std::filesystem::path ShaderCaptureDir(const std::filesystem::path& dir, std::string_view game_id, const std::tm& t);

// A draw, or an occlusion query report (Xenia's ZPD observer, patch 0021) in command order: for kZpdQueued and
// kZpdRetired vs_hash is the report handle, vs_mod the guest address, ps_hash/ps_mod the ZPass/ZFail samples (retired);
// a kCopy (resolve) has RB_COPY_CONTROL, RB_COPY_DEST_BASE, RB_COPY_DEST_INFO, RB_COPY_DEST_PITCH in the four hashes,
// RB_DEPTH_INFO in depth_info and RB_COLOR_INFO in color_mask.
struct DrawRecord {
  enum class Kind : uint8_t { kDraw, kZpdQueued, kZpdRetired, kCopy };
  uint64_t vs_hash, vs_mod;
  uint64_t ps_hash, ps_mod;  // ps_hash 0: no pixel shader
  bool skipped;              // not issued (pipeline not ready)
  Kind kind = Kind::kDraw;
  // Draw state: RB_DEPTHCONTROL, PA_SU_SC_MODE_CNTL (culling), RB_COLOR_MASK, RB_DEPTH_INFO.
  uint32_t depth_control = 0, mode_control = 0, color_mask = 0, depth_info = 0;
  // More registers (fetch constant 0, viewport, surface, shader constants: list in d3d12_backend.cpp), written as
  // "  st <32 x 8 hex>" after the draw line when they differ from the previous draw's.
  std::array<uint32_t, 32> regs{};
};

// Text of frames/<index>.txt: "frame 00001 host 1234 ms 200", then one line per record:
// "draw <n> vs <hash>:<mod> ps <hash>:<mod>|- dc <8 hex> sc <8 hex> cm <8 hex> di <8 hex>[ skipped]" (16 uppercase hex digits,
// n counts draws only),
// "zpd <handle> addr <8 hex> queued", "zpd <handle> addr <8 hex> zpass <n> zfail <n>".
std::string FormatFrameList(uint32_t index, uint64_t host_frame, uint32_t ms, std::span<const DrawRecord> draws);

// Thread-safe; Active() is the only cost per draw when no recording is running.
class ShaderCaptureSession {
 public:
  static constexpr std::chrono::milliseconds kShotInterval{200};

  // Creates <dir>/shaders and <dir>/frames (<dir>_2, _3, ... if <dir> exists); no-op if already active.
  void Start(std::filesystem::path dir, std::chrono::steady_clock::time_point now);
  void Stop();
  bool Active() const { return active_.load(std::memory_order_relaxed); }
  // Valid while active (and after Stop until the next Start).
  const std::filesystem::path& Dir() const { return dir_; }
  // True once per (hash, mod) per recording.
  bool FirstSeen(uint64_t hash, uint64_t mod);
  void NoteDraw(const DrawRecord& d);
  // Draws noted since the last call.
  std::vector<DrawRecord> TakeFrame();
  // Due at Start and then once kShotInterval has passed since the last due shot: {index from 1, ms since Start}.
  std::optional<std::pair<uint32_t, uint32_t>> ShotDue(std::chrono::steady_clock::time_point now);
  uint32_t ShadersSaved() const;

 private:
  std::atomic<bool> active_{false};
  mutable std::mutex mutex_;
  std::filesystem::path dir_;
  std::chrono::steady_clock::time_point start_, next_shot_;
  uint32_t shots_ = 0;
  std::set<std::pair<uint64_t, uint64_t>> seen_;
  std::vector<DrawRecord> frame_;
};

}  // namespace gpu
