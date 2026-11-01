// Shader capture without a GPU (gpu/shader_capture.h): folder of a recording, text of a frame's draw list,
// session state (shaders seen once per (hash, modification), shots every 200 ms, draws of the frame).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <vector>

#include "gpu/shader_capture.h"
#include "temp_dir.h"

namespace {

using namespace std::chrono_literals;

std::tm Tm(int y, int mo, int d, int h, int mi, int s) {
  std::tm t{};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  return t;
}

}  // namespace

TEST_CASE("shader_capture: folder <dir>/shader_captures/<game>/<date_time>", "[shader_capture]") {
  CHECK(gpu::ShaderCaptureDir(LR"(C:\g)", "gow3", Tm(2026, 10, 31, 21, 14, 3)) ==
        std::filesystem::path(LR"(C:\g\shader_captures\gow3\2026-10-31_21-14-03)"));
}

TEST_CASE("shader_capture: frame list text, draw without PS and skipped draw", "[shader_capture]") {
  const std::vector<gpu::DrawRecord> draws{
      {0x0123456789ABCDEFull, 0, 0xFEDCBA9876543210ull, 3, false},
      {0x0123456789ABCDEFull, 0, 0, 0, true},
  };
  CHECK(gpu::FormatFrameList(1, 1234, 200, draws) ==
        "frame 00001 host 1234 ms 200\n"
        "draw 0 vs 0123456789ABCDEF:0000000000000000 ps FEDCBA9876543210:0000000000000003 dc 00000000 sc 00000000 cm 00000000 di 00000000\n"
        "draw 1 vs 0123456789ABCDEF:0000000000000000 ps - dc 00000000 sc 00000000 cm 00000000 di 00000000 skipped\n");
}

TEST_CASE("shader_capture: Start creates shaders/ and frames/, FirstSeen once per (hash, mod) per session",
          "[shader_capture]") {
  gow2_test::TempDir tmp("shader_capture");
  gpu::ShaderCaptureSession s;
  CHECK_FALSE(s.Active());
  const auto t0 = std::chrono::steady_clock::now();
  s.Start(tmp.path / "rec", t0);
  CHECK(s.Active());
  CHECK(s.Dir() == tmp.path / "rec");
  CHECK(std::filesystem::is_directory(tmp.path / "rec" / "shaders"));
  CHECK(std::filesystem::is_directory(tmp.path / "rec" / "frames"));
  CHECK(s.FirstSeen(1, 0));
  CHECK_FALSE(s.FirstSeen(1, 0));
  CHECK(s.FirstSeen(1, 3));
  CHECK(s.ShadersSaved() == 2);
  s.Start(tmp.path / "other", t0);  // already active: no-op
  CHECK(s.Dir() == tmp.path / "rec");
  s.Stop();
  CHECK_FALSE(s.Active());
  s.Start(tmp.path / "rec2", t0);
  CHECK(s.FirstSeen(1, 0));
  CHECK(s.ShadersSaved() == 1);
}

TEST_CASE("shader_capture: a shot at Start, then one every 200 ms", "[shader_capture]") {
  gow2_test::TempDir tmp("shader_capture");
  gpu::ShaderCaptureSession s;
  const auto t0 = std::chrono::steady_clock::now();
  s.Start(tmp.path, t0);
  CHECK(s.ShotDue(t0) == std::pair<uint32_t, uint32_t>{1, 0});
  CHECK_FALSE(s.ShotDue(t0 + 100ms));
  CHECK(s.ShotDue(t0 + 200ms) == std::pair<uint32_t, uint32_t>{2, 200});
  CHECK_FALSE(s.ShotDue(t0 + 300ms));
  s.Stop();
  s.Start(tmp.path, t0 + 1s);
  CHECK(s.ShotDue(t0 + 1s) == std::pair<uint32_t, uint32_t>{1, 0});
}

TEST_CASE("shader_capture: TakeFrame returns the draws in order, then empty", "[shader_capture]") {
  gow2_test::TempDir tmp("shader_capture");
  gpu::ShaderCaptureSession s;
  s.Start(tmp.path, std::chrono::steady_clock::now());
  s.NoteDraw({1, 0, 2, 0, false});
  s.NoteDraw({3, 0, 0, 0, true});
  const auto f = s.TakeFrame();
  REQUIRE(f.size() == 2);
  CHECK(f[0].vs_hash == 1);
  CHECK(f[1].skipped);
  CHECK(s.TakeFrame().empty());
}

TEST_CASE("shader_capture: a second recording in the same folder gets a _2 suffix", "[shader_capture]") {
  gow2_test::TempDir tmp("shader_capture");
  gpu::ShaderCaptureSession s;
  const auto t0 = std::chrono::steady_clock::now();
  s.Start(tmp.path / "rec", t0);
  s.Stop();
  s.Start(tmp.path / "rec", t0);
  CHECK(s.Dir() == tmp.path / "rec_2");
  s.Stop();
  s.Start(tmp.path / "rec", t0);
  CHECK(s.Dir() == tmp.path / "rec_3");
}

TEST_CASE("shader_capture: frame list with ZPD reports, draws numbered apart", "[shader_capture]") {
  using K = gpu::DrawRecord::Kind;
  const std::vector<gpu::DrawRecord> draws{
      {7, 0x1A2B3C40, 0, 0, false, K::kZpdQueued},
      {0x0123456789ABCDEFull, 0, 0, 0, false},
      {6, 0x1A2B3C20, 12, 3, false, K::kZpdRetired},
      {0x0123456789ABCDEFull, 0, 0, 0, false, K::kDraw, 0x00700052, 0x00080204, 0xF},
  };
  CHECK(gpu::FormatFrameList(2, 10, 400, draws) ==
        "frame 00002 host 10 ms 400\n"
        "zpd 7 addr 1A2B3C40 queued\n"
        "draw 0 vs 0123456789ABCDEF:0000000000000000 ps - dc 00000000 sc 00000000 cm 00000000 di 00000000\n"
        "zpd 6 addr 1A2B3C20 zpass 12 zfail 3\n"
        "draw 1 vs 0123456789ABCDEF:0000000000000000 ps - dc 00700052 sc 00080204 cm 0000000F di 00000000\n");
}

TEST_CASE("shader_capture: frame list with a resolve", "[shader_capture]") {
  gpu::DrawRecord copy{0x300, 0x13D6C000, 0x1A0, 0x2D00500, false, gpu::DrawRecord::Kind::kCopy};
  copy.depth_info = 0x10000;
  copy.color_mask = 0x5A0;
  const std::vector<gpu::DrawRecord> draws{copy};
  CHECK(gpu::FormatFrameList(1, 2, 3, draws) ==
        "frame 00001 host 2 ms 3\n"
        "copy ctl 00000300 dest 13D6C000 info 000001A0 pitch 02D00500 di 00010000 ci 000005A0\n");
}

TEST_CASE("shader_capture: frame list writes the extra registers only when they change", "[shader_capture]") {
  using K = gpu::DrawRecord::Kind;
  std::array<uint32_t, 32> regs{};
  regs[0] = 0xAB;
  regs[31] = 0x12345678;
  const std::vector<gpu::DrawRecord> draws{
      {1, 0, 0, 0, false, K::kDraw, 0, 0, 0, 0, regs},
      {2, 0, 0, 0, false, K::kDraw, 0, 0, 0, 0, regs},
      {3, 0, 0, 0, false},
  };
  std::string st = "  st 000000AB";
  for (int r = 1; r < 31; ++r) st += " 00000000";
  st += " 12345678\n";
  std::string zero = "  st";
  for (int r = 0; r < 32; ++r) zero += " 00000000";
  zero += "\n";
  const std::string tail = ":0000000000000000 ps - dc 00000000 sc 00000000 cm 00000000 di 00000000\n";
  CHECK(gpu::FormatFrameList(0, 0, 0, draws) == "frame 00000 host 0 ms 0\n"
                                                "draw 0 vs 0000000000000001" + tail + st +
                                                "draw 1 vs 0000000000000002" + tail +
                                                "draw 2 vs 0000000000000003" + tail + zero);
}
