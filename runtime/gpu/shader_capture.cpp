// See shader_capture.h.
#include "gpu/shader_capture.h"

#include <cstdio>
#include <system_error>

namespace gpu {

std::filesystem::path ShaderCaptureDir(const std::filesystem::path& dir, std::string_view game_id, const std::tm& t) {
  char stamp[32];
  std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
  return dir / "shader_captures" / std::string(game_id) / stamp;
}

std::string FormatFrameList(uint32_t index, uint64_t host_frame, uint32_t ms, std::span<const DrawRecord> draws) {
  char line[128];
  std::snprintf(line, sizeof line, "frame %05u host %llu ms %u\n", index, static_cast<unsigned long long>(host_frame),
                ms);
  std::string out = line;
  size_t i = 0;
  std::array<uint32_t, 32> prev_regs{};
  for (const DrawRecord& d : draws) {
    if (d.kind != DrawRecord::Kind::kDraw) {
      if (d.kind == DrawRecord::Kind::kCopy)
        std::snprintf(line, sizeof line, "copy ctl %08llX dest %08llX info %08llX pitch %08llX di %08X ci %08X\n",
                      static_cast<unsigned long long>(d.vs_hash), static_cast<unsigned long long>(d.vs_mod),
                      static_cast<unsigned long long>(d.ps_hash), static_cast<unsigned long long>(d.ps_mod),
                      d.depth_info, d.color_mask);
      else if (d.kind == DrawRecord::Kind::kZpdQueued)
        std::snprintf(line, sizeof line, "zpd %llu addr %08llX queued\n", static_cast<unsigned long long>(d.vs_hash),
                      static_cast<unsigned long long>(d.vs_mod));
      else
        std::snprintf(line, sizeof line, "zpd %llu addr %08llX zpass %llu zfail %llu\n",
                      static_cast<unsigned long long>(d.vs_hash), static_cast<unsigned long long>(d.vs_mod),
                      static_cast<unsigned long long>(d.ps_hash), static_cast<unsigned long long>(d.ps_mod));
      out += line;
      continue;
    }
    std::snprintf(line, sizeof line, "draw %zu vs %016llX:%016llX ps ", i++, static_cast<unsigned long long>(d.vs_hash),
                  static_cast<unsigned long long>(d.vs_mod));
    out += line;
    if (d.ps_hash) {
      std::snprintf(line, sizeof line, "%016llX:%016llX", static_cast<unsigned long long>(d.ps_hash),
                    static_cast<unsigned long long>(d.ps_mod));
      out += line;
    } else {
      out += '-';
    }
    std::snprintf(line, sizeof line, " dc %08X sc %08X cm %08X di %08X", d.depth_control, d.mode_control,
                  d.color_mask, d.depth_info);
    out += line;
    out += d.skipped ? " skipped\n" : "\n";
    if (d.regs != prev_regs) {
      out += "  st";
      for (uint32_t r : d.regs) {
        std::snprintf(line, sizeof line, " %08X", r);
        out += line;
      }
      out += '\n';
      prev_regs = d.regs;
    }
  }
  return out;
}

void ShaderCaptureSession::Start(std::filesystem::path dir, std::chrono::steady_clock::time_point now) {
  std::lock_guard g(mutex_);
  if (active_.load()) return;
  std::error_code ec;
  // Two recordings in the same second (the folder name's resolution): _2, _3, ... instead of overwriting.
  const std::filesystem::path base = dir;
  for (uint32_t n = 2; std::filesystem::exists(dir, ec); ++n) dir = base.native() + L"_" + std::to_wstring(n);
  std::filesystem::create_directories(dir / "shaders", ec);
  std::filesystem::create_directories(dir / "frames", ec);
  dir_ = std::move(dir);
  start_ = next_shot_ = now;
  shots_ = 0;
  seen_.clear();
  frame_.clear();
  active_.store(true);
}

void ShaderCaptureSession::Stop() {
  std::lock_guard g(mutex_);
  active_.store(false);
}

bool ShaderCaptureSession::FirstSeen(uint64_t hash, uint64_t mod) {
  std::lock_guard g(mutex_);
  return seen_.emplace(hash, mod).second;
}

void ShaderCaptureSession::NoteDraw(const DrawRecord& d) {
  std::lock_guard g(mutex_);
  frame_.push_back(d);
}

std::vector<DrawRecord> ShaderCaptureSession::TakeFrame() {
  std::lock_guard g(mutex_);
  return std::exchange(frame_, {});
}

std::optional<std::pair<uint32_t, uint32_t>> ShaderCaptureSession::ShotDue(std::chrono::steady_clock::time_point now) {
  std::lock_guard g(mutex_);
  if (now < next_shot_) return std::nullopt;
  // From the due instant, not from now: a late frame does not shift the following shots.
  while (next_shot_ <= now) next_shot_ += kShotInterval;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
  return std::pair{++shots_, uint32_t(ms)};
}

uint32_t ShaderCaptureSession::ShadersSaved() const {
  std::lock_guard g(mutex_);
  return uint32_t(seen_.size());
}

}  // namespace gpu
