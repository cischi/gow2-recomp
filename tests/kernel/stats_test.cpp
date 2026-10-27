// Exit summary counters (runtime/kernel/stats.h).
#include "kernel/stats.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "../temp_dir.h"

// operator new counter for the "no allocation" test: it counts only the
// allocations of the thread that turned t_count_allocs on, and only while it is on
// (the process's other threads, Catch2 included, do not enter the count).
namespace {
thread_local bool t_count_allocs = false;
std::atomic<uint64_t> g_counted_allocs{0};

void* CountedAlloc(std::size_t n) {
  if (t_count_allocs) g_counted_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t n) { return CountedAlloc(n); }
void* operator new[](std::size_t n) { return CountedAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return CountedAlloc(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return CountedAlloc(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

bool Contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// Value of `[summary] <name>=<n>` in the summary. The counters are global for
// the process and the TEST_CASEs may all run in a single process: the counter
// cases check variations, not absolute values.
uint64_t Counter(const std::string& s, const std::string& name) {
  const std::string key = "[summary] " + name + "=";
  const size_t pos = s.find(key);
  REQUIRE(pos != std::string::npos);
  return std::stoull(s.substr(pos + key.size()));
}
std::vector<uint64_t> Counters(const std::vector<std::string>& names) {
  const std::string s = kernel::stats::Summary();
  std::vector<uint64_t> v;
  for (const std::string& name : names) v.push_back(Counter(s, name));
  return v;
}
// The `names` lines are contiguous, in this order, in the summary.
void CheckContiguous(const std::string& s, const std::vector<std::string>& names) {
  INFO(s);
  size_t pos = s.find("[summary] " + names.front() + "=");
  REQUIRE(pos != std::string::npos);
  for (size_t k = 1; k < names.size(); ++k) {
    pos = s.find('\n', pos);
    REQUIRE(pos != std::string::npos);
    ++pos;
    const std::string key = "[summary] " + names[k] + "=";
    CHECK(s.compare(pos, key.size(), key) == 0);
  }
}

}  // namespace

TEST_CASE("stats: UNIMPLEMENTED in the log only on the first call, with r3..r10") {
  const auto path = gow2_test::UniqueTempPath("gow2_stats_test");
  LogInit(path);
  const std::array<uint64_t, 8> regs = {1, 2, 3, 4, 5, 6, 7, 0xABCDEF0123};
  kernel::stats::NoteUnimplemented("XamLogOnce", regs);
  kernel::stats::NoteUnimplemented("XamLogOnce", regs);
  kernel::stats::NoteUnimplemented("XamNoRegs");
  LogInit({});  // closes the file
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(path);
  CHECK(ss.str() ==
        "[kernel] UNIMPLEMENTED XamLogOnce(r3=0x1, r4=0x2, r5=0x3, r6=0x4, r7=0x5, r8=0x6, r9=0x7, r10=0xABCDEF0123)\n"
        "[kernel] UNIMPLEMENTED XamNoRegs\n");
  CHECK(kernel::stats::Summary().find("[summary] unimplemented XamLogOnce=2\n") != std::string::npos);
}

// A single TEST_CASE for the counts: the counters are global for the process.
TEST_CASE("stats: counts, ordering and first 10 packages in the summary") {
  for (int i = 0; i < 3; ++i) kernel::stats::NoteUnimplemented("XamFoo");
  kernel::stats::NoteUnimplemented("KeBar");
  kernel::stats::NoteUnimplemented("TieB");
  kernel::stats::NoteUnimplemented("TieA");
  for (int i = 0; i < 12; ++i) kernel::stats::NotePackageOpened("game:\\pkg" + std::to_string(i) + ".xxx");
  // cooked_packages_opened: only .xxx under GearGame\CookedXenon (any case and separators)
  kernel::stats::NotePackageOpened("game:\\GearGame\\CookedXenon\\Startup.xxx");
  kernel::stats::NotePackageOpened("\\Device\\Cdrom0/geargame/COOKEDXENON/sub/b.XXX");
  kernel::stats::NotePackageOpened("GearGame\\CookedXenon\\f.xxx");
  kernel::stats::NotePackageOpened("game:\\GearGame\\CookedXenon\\c.upk");
  kernel::stats::NotePackageOpened("game:\\NotGearGame\\CookedXenon\\d.xxx");
  kernel::stats::NotePackageOpened("game:\\GearGame\\CookedXenonX\\e.xxx");
  kernel::stats::NotePackageOpened("game:\\GearGame\\CookedXenon.xxx");

  // unknown PM4 packets of the fake GPU: by descending count
  for (int i = 0; i < 5; ++i) kernel::stats::NoteGpuUnknownPacket(0x22);
  kernel::stats::NoteGpuUnknownPacket(0x2D);
  kernel::stats::NoteGpuUnknownPacket(0x2D);
  kernel::stats::NoteGpuUnknownPacket(0x1FF);  // beyond 0x7F: ignored

  // from several threads at once: no count lost
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([] {
      for (int i = 0; i < 1000; ++i) {
        kernel::stats::NoteUnimplemented("NtBaz");
        kernel::stats::NoteFramePresented();
      }
      kernel::stats::NoteThreadCreated();
    });
  for (auto& t : threads) t.join();

  const std::string s = kernel::stats::Summary();
  INFO(s);
  CHECK(Contains(s, "[summary] unimplemented NtBaz=8000\n"));
  CHECK(Contains(s, "[summary] unimplemented XamFoo=3\n"));
  CHECK(Contains(s, "[summary] unimplemented KeBar=1\n"));
  CHECK(s.find("NtBaz=") < s.find("XamFoo="));  // by descending number of calls
  CHECK(s.find("XamFoo=") < s.find("KeBar="));
  CHECK(Contains(s, "[summary] packages_opened=19\n"));
  CHECK(Contains(s, "[summary] cooked_packages_opened=3\n"));
  CHECK(Contains(s, "[summary] package game:\\pkg0.xxx\n"));
  CHECK(Contains(s, "[summary] package game:\\pkg9.xxx\n"));
  CHECK_FALSE(Contains(s, "pkg10.xxx"));
  CHECK(Contains(s, "[summary] frames_presented=8000\n"));
  CHECK(Contains(s, "[summary] threads_created=8\n"));

  CHECK(Contains(s, "[summary] gpu_unknown_packet 0x22=5\n[summary] gpu_unknown_packet 0x2D=2\n"));
  CHECK(s.find("gpu_unknown_packet") > s.find("unimplemented TieB"));
  CHECK(s.find("gpu_unknown_packet") < s.find("packages_opened="));
  CHECK_FALSE(Contains(s, "0x1FF"));

  // exact format of packages/frames/threads, after the unimplemented lines
  std::string tail = "[summary] packages_opened=19\n";
  for (int i = 0; i < 10; ++i) tail += "[summary] package game:\\pkg" + std::to_string(i) + ".xxx\n";
  tail += "[summary] last_package game:\\GearGame\\CookedXenon.xxx\n";  // the last one opened, even if it is not among the first 10
  tail += "[summary] cooked_packages_opened=3\n";
  tail += "[summary] frames_presented=8000\n";
  tail += "[summary] gpu_draws=0\n[summary] gpu_draws_skipped=0\n[summary] gpu_resolves=0\n";
  tail += "[summary] gpu_edram_transfers=0\n";
  tail += "[summary] host_frames_presented=0\n";
  tail += "[summary] audio_callbacks=0\n[summary] audio_frames_submitted=0\n[summary] xma_kicks=0\n";
  tail += "[summary] xma_frames_decoded=0\n[summary] xma_decode_errors=0\n";
  tail += "[summary] input_get_state_calls=0\n[summary] input_injected_events=0\n";
  tail += "[summary] threads_created=8\n";
  CHECK(s.size() > tail.size());
  CHECK(s.substr(s.size() - tail.size()) == tail);
  // with equal calls, by name
  CHECK(s.find("unimplemented TieA=1\n") != std::string::npos);
  CHECK(s.find("unimplemented TieA=1\n") < s.find("unimplemented TieB=1\n"));

  // WriteSummary: lines in the log, only once
  const auto path = gow2_test::UniqueTempPath("gow2_stats_summary");
  LogInit(path);
  CHECK(kernel::stats::WriteSummary());
  CHECK_FALSE(kernel::stats::WriteSummary());
  LogInit({});
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(path);
  CHECK(ss.str() == s);

  CHECK_FALSE(kernel::stats::Trace());
  kernel::stats::SetTrace(true);
  CHECK(kernel::stats::Trace());
  kernel::stats::SetTrace(false);
}

// Xenia GPU counters: atomic, no lock; lines right after
// frames_presented. gpu_edram_transfers: EDRAM ownership transfers of draws (patch 0017).
TEST_CASE("stats: draws, skipped draws, resolves, EDRAM transfers and frames presented by the host") {
  const std::vector<std::string> names = {"frames_presented", "gpu_draws", "gpu_draws_skipped", "gpu_resolves",
                                          "gpu_edram_transfers", "host_frames_presented"};
  const std::vector<uint64_t> before = Counters(names);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([t] {
      for (int i = 0; i < 1000; ++i) {
        kernel::stats::NoteGpuDraw(i % 4 != 0);  // 250 skipped per thread
        if (i % 10 == 0) kernel::stats::NoteGpuResolve();
        if (i % 100 == 0) kernel::stats::NoteGpuEdramTransfers(3);
      }
      if (t == 0) kernel::stats::NoteHostFramePresented();
    });
  for (auto& t : threads) t.join();
  kernel::stats::NoteFramePresented();
  const std::vector<uint64_t> after = Counters(names);
  const std::vector<uint64_t> expected = {1, 4000, 1000, 400, 120, 1};
  for (size_t k = 0; k < names.size(); ++k) {
    INFO(names[k]);
    CHECK(after[k] - before[k] == expected[k]);
  }
  std::vector<std::string> order = names;
  order.push_back("audio_callbacks");
  CheckContiguous(kernel::stats::Summary(), order);
}

// Audio/XMA/input counters (sub-project 4): atomic, no lock; lines
// between host_frames_presented and threads_created, in the order of the Global
// Constraints.
TEST_CASE("stats: audio, XMA and input counters in the summary") {
  const std::vector<std::string> names = {"audio_callbacks",       "audio_frames_submitted", "xma_kicks",
                                          "xma_frames_decoded",    "xma_decode_errors",      "input_get_state_calls",
                                          "input_injected_events"};
  const std::vector<uint64_t> before = Counters(names);
  for (int i = 0; i < 3; ++i) {
    kernel::stats::NoteAudioCallback();
    kernel::stats::NoteAudioFrameSubmitted();
    kernel::stats::NoteXmaFrameDecoded();
    kernel::stats::NoteXmaDecodeError();
    kernel::stats::NoteInputGetState();
    kernel::stats::NoteInputInjectedEvent();
  }
  kernel::stats::NoteXmaKicks(5);
  const std::vector<uint64_t> after = Counters(names);
  const std::vector<uint64_t> expected = {3, 3, 5, 3, 3, 3, 3};
  for (size_t k = 0; k < names.size(); ++k) {
    INFO(names[k]);
    CHECK(after[k] - before[k] == expected[k]);
  }
  std::vector<std::string> order = {"host_frames_presented"};
  order.insert(order.end(), names.begin(), names.end());
  order.push_back("threads_created");
  CheckContiguous(kernel::stats::Summary(), order);
}

TEST_CASE("stats: NoteUnimplemented on a slot, 8 threads x 100000 calls, exact count") {
  static kernel::stats::UnimplementedSlot slot{"SlotConcurrent"};
  const std::array<uint64_t, 8> regs{};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < 100000; ++i) kernel::stats::NoteUnimplemented(slot, regs);
    });
  for (auto& t : threads) t.join();
  CHECK(slot.calls.load() == 800000);
  const std::string s = kernel::stats::Summary();
  INFO(s);
  CHECK(Contains(s, "[summary] unimplemented SlotConcurrent=800000\n"));
}

TEST_CASE("stats: NoteUnimplemented on a slot logs once and after the first call does not allocate") {
  // names beyond the small string optimization: a std::string per call would allocate
  static kernel::stats::UnimplementedSlot a{"SlotNoAllocationsInHotPathA"};
  static kernel::stats::UnimplementedSlot b{"SlotNoAllocationsInHotPathB"};
  const auto path = gow2_test::UniqueTempPath("gow2_stats_slot");
  LogInit(path);
  const std::array<uint64_t, 8> regs = {1, 2, 3, 4, 5, 6, 7, 8};
  kernel::stats::NoteUnimplemented(a, regs);  // first call: registers and logs (may allocate)
  kernel::stats::NoteUnimplemented(b, regs);
  g_counted_allocs = 0;
  t_count_allocs = true;
  for (int i = 0; i < 10000; ++i) {
    kernel::stats::NoteUnimplemented(a, regs);
    kernel::stats::NoteUnimplemented(b, regs);
  }
  t_count_allocs = false;
  const uint64_t allocs = g_counted_allocs.load();
  // the counter works: an allocation on the test thread shows up
  t_count_allocs = true;
  void* volatile probe = ::operator new(16);
  t_count_allocs = false;
  ::operator delete(probe);
  const uint64_t probe_allocs = g_counted_allocs.load() - allocs;
  LogInit({});
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(path);

  CHECK(allocs == 0);
  CHECK(probe_allocs == 1);
  CHECK(a.calls.load() == 10001);
  CHECK(b.calls.load() == 10001);
  CHECK(ss.str() ==
        "[kernel] UNIMPLEMENTED SlotNoAllocationsInHotPathA(r3=0x1, r4=0x2, r5=0x3, r6=0x4, r7=0x5, r8=0x6, r9=0x7, r10=0x8)\n"
        "[kernel] UNIMPLEMENTED SlotNoAllocationsInHotPathB(r3=0x1, r4=0x2, r5=0x3, r6=0x4, r7=0x5, r8=0x6, r9=0x7, r10=0x8)\n");
  const std::string s = kernel::stats::Summary();
  CHECK(Contains(s, "[summary] unimplemented SlotNoAllocationsInHotPathA=10001\n"));
  CHECK(Contains(s, "[summary] unimplemented SlotNoAllocationsInHotPathB=10001\n"));
}
