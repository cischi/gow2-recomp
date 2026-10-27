#include "kernel/stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imports.h"
#include "kernel/function.h"
#include "log.h"

namespace kernel::stats {

namespace {

constexpr size_t kPackagesListed = 10;

std::mutex g_mutex;
std::unordered_map<std::string, uint64_t> g_unimplemented;  // under g_mutex
std::vector<UnimplementedSlot*> g_slots;                     // under g_mutex, registered slots
std::vector<std::string> g_packages;                         // under g_mutex, first kPackagesListed
uint64_t g_packages_opened = 0;                              // under g_mutex
uint64_t g_cooked_packages_opened = 0;                       // under g_mutex
std::string g_last_package;                                  // under g_mutex, the last one opened
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_threads{0};
std::atomic<uint64_t> g_gpu_draws{0};
std::atomic<uint64_t> g_gpu_draws_skipped{0};
std::atomic<uint64_t> g_gpu_resolves{0};
std::atomic<uint64_t> g_gpu_edram_transfers{0};
std::atomic<uint64_t> g_host_frames{0};
std::atomic<uint64_t> g_audio_callbacks{0};
std::atomic<uint64_t> g_audio_frames_submitted{0};
std::atomic<uint64_t> g_xma_kicks{0};
std::atomic<uint64_t> g_xma_frames_decoded{0};
std::atomic<uint64_t> g_xma_decode_errors{0};
std::atomic<uint64_t> g_input_get_state{0};
std::atomic<uint64_t> g_input_injected{0};
std::array<std::atomic<uint64_t>, 128> g_gpu_unknown{};
std::atomic<bool> g_trace{false};

// true only on the first call for `name`.
bool CountUnimplemented(const char* name) {
  std::lock_guard lock(g_mutex);
  return g_unimplemented[name]++ == 0;
}

// .xxx under GearGame\CookedXenon (at any depth), case-
// insensitive and with '\' or '/' as separator.
bool IsCookedPackage(std::string_view guest_path) {
  std::string p(guest_path);
  for (char& c : p) c = (c == '/') ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
  if (p.size() < 4 || p.compare(p.size() - 4, 4, ".xxx") != 0) return false;
  constexpr std::string_view kDir = "geargame\\cookedxenon\\";
  for (size_t pos = p.find(kDir); pos != std::string::npos; pos = p.find(kDir, pos + 1))
    if (pos == 0 || p[pos - 1] == '\\' || p[pos - 1] == ':') return true;
  return false;
}

}  // namespace

void NoteUnimplemented(const char* name) {
  if (CountUnimplemented(name)) LogLine(std::string("[kernel] UNIMPLEMENTED ") + name);
}

void NoteUnimplemented(const char* name, std::span<const uint64_t, 8> r3_to_r10) {
  if (CountUnimplemented(name)) LogLine(FormatImportCall(std::string("UNIMPLEMENTED ") + name, r3_to_r10));
}

void NoteUnimplemented(UnimplementedSlot& slot, std::span<const uint64_t, 8> r3_to_r10) {
  slot.calls.fetch_add(1, std::memory_order_relaxed);
  if (slot.state.load(std::memory_order_acquire) != 0) return;
  uint32_t expected = 0;
  if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
  // only the first thread gets here; those calling in the meantime have already counted
  {
    std::lock_guard lock(g_mutex);
    g_slots.push_back(&slot);
  }
  slot.state.store(2, std::memory_order_release);
  LogLine(FormatImportCall(std::string("UNIMPLEMENTED ") + slot.name, r3_to_r10));
}

void NotePackageOpened(std::string_view guest_path) {
  const bool cooked = IsCookedPackage(guest_path);
  std::lock_guard lock(g_mutex);
  ++g_packages_opened;
  if (cooked) ++g_cooked_packages_opened;
  if (g_packages.size() < kPackagesListed) g_packages.emplace_back(guest_path);
  g_last_package.assign(guest_path);
}

void NoteFramePresented() { g_frames.fetch_add(1, std::memory_order_relaxed); }
void NoteThreadCreated() { g_threads.fetch_add(1, std::memory_order_relaxed); }
void NoteGpuDraw(bool ok) {
  g_gpu_draws.fetch_add(1, std::memory_order_relaxed);
  if (!ok) g_gpu_draws_skipped.fetch_add(1, std::memory_order_relaxed);
}
void NoteGpuResolve() { g_gpu_resolves.fetch_add(1, std::memory_order_relaxed); }
void NoteGpuEdramTransfers(uint64_t count) { g_gpu_edram_transfers.fetch_add(count, std::memory_order_relaxed); }
void NoteHostFramePresented() { g_host_frames.fetch_add(1, std::memory_order_relaxed); }
void NoteAudioCallback() { g_audio_callbacks.fetch_add(1, std::memory_order_relaxed); }
void NoteAudioFrameSubmitted() { g_audio_frames_submitted.fetch_add(1, std::memory_order_relaxed); }
void NoteXmaKicks(uint32_t contexts) { g_xma_kicks.fetch_add(contexts, std::memory_order_relaxed); }
void NoteXmaFrameDecoded() { g_xma_frames_decoded.fetch_add(1, std::memory_order_relaxed); }
void NoteXmaDecodeError() { g_xma_decode_errors.fetch_add(1, std::memory_order_relaxed); }
void NoteInputGetState() { g_input_get_state.fetch_add(1, std::memory_order_relaxed); }
void NoteInputInjectedEvent() { g_input_injected.fetch_add(1, std::memory_order_relaxed); }
void NoteGpuUnknownPacket(uint32_t opcode) {
  if (opcode < g_gpu_unknown.size()) g_gpu_unknown[opcode].fetch_add(1, std::memory_order_relaxed);
}

std::string Summary() {
  std::vector<std::pair<std::string, uint64_t>> unimplemented;
  std::vector<std::string> packages;
  std::string last_package;
  uint64_t packages_opened, cooked_packages_opened;
  {
    std::lock_guard lock(g_mutex);
    std::unordered_map<std::string, uint64_t> merged = g_unimplemented;
    for (const UnimplementedSlot* slot : g_slots) merged[slot->name] += slot->calls.load(std::memory_order_relaxed);
    unimplemented.assign(merged.begin(), merged.end());
    packages = g_packages;
    last_package = g_last_package;
    packages_opened = g_packages_opened;
    cooked_packages_opened = g_cooked_packages_opened;
  }
  std::sort(unimplemented.begin(), unimplemented.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  std::string s;
  for (const auto& [name, count] : unimplemented)
    s += "[summary] unimplemented " + name + "=" + std::to_string(count) + "\n";
  std::vector<std::pair<uint32_t, uint64_t>> gpu;
  for (uint32_t op = 0; op < g_gpu_unknown.size(); ++op)
    if (const uint64_t n = g_gpu_unknown[op].load()) gpu.emplace_back(op, n);
  std::stable_sort(gpu.begin(), gpu.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [op, n] : gpu) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[summary] gpu_unknown_packet 0x%02X=%llu\n", op, static_cast<unsigned long long>(n));
    s += buf;
  }
  s += "[summary] packages_opened=" + std::to_string(packages_opened) + "\n";
  for (const auto& p : packages) s += "[summary] package " + p + "\n";
  if (!last_package.empty()) s += "[summary] last_package " + last_package + "\n";
  s += "[summary] cooked_packages_opened=" + std::to_string(cooked_packages_opened) + "\n";
  s += "[summary] frames_presented=" + std::to_string(g_frames.load()) + "\n";
  s += "[summary] gpu_draws=" + std::to_string(g_gpu_draws.load()) + "\n";
  s += "[summary] gpu_draws_skipped=" + std::to_string(g_gpu_draws_skipped.load()) + "\n";
  s += "[summary] gpu_resolves=" + std::to_string(g_gpu_resolves.load()) + "\n";
  s += "[summary] gpu_edram_transfers=" + std::to_string(g_gpu_edram_transfers.load()) + "\n";
  s += "[summary] host_frames_presented=" + std::to_string(g_host_frames.load()) + "\n";
  s += "[summary] audio_callbacks=" + std::to_string(g_audio_callbacks.load()) + "\n";
  s += "[summary] audio_frames_submitted=" + std::to_string(g_audio_frames_submitted.load()) + "\n";
  s += "[summary] xma_kicks=" + std::to_string(g_xma_kicks.load()) + "\n";
  s += "[summary] xma_frames_decoded=" + std::to_string(g_xma_frames_decoded.load()) + "\n";
  s += "[summary] xma_decode_errors=" + std::to_string(g_xma_decode_errors.load()) + "\n";
  s += "[summary] input_get_state_calls=" + std::to_string(g_input_get_state.load()) + "\n";
  s += "[summary] input_injected_events=" + std::to_string(g_input_injected.load()) + "\n";
  s += "[summary] threads_created=" + std::to_string(g_threads.load()) + "\n";
  return s;
}

namespace {
std::atomic<void (*)()> g_before_summary{nullptr};
}  // namespace

void SetBeforeSummaryHook(void (*hook)()) { g_before_summary.store(hook, std::memory_order_release); }

bool WriteSummary() {
  static std::mutex write_mutex;  // distinct from g_mutex: Summary() takes it
  static bool written = false;
  std::lock_guard lock(write_mutex);
  if (written) return false;
  written = true;
  if (void (*hook)() = g_before_summary.load(std::memory_order_acquire)) hook();
  const std::string summary = Summary();
  for (size_t pos = 0, end; pos < summary.size(); pos = end + 1) {
    end = summary.find('\n', pos);
    if (end == std::string::npos) end = summary.size();
    LogLine(std::string_view(summary).substr(pos, end - pos));
  }
  return true;
}

void TraceCall(const char* name, const PPCContext& ctx) {
  const std::array<uint64_t, 8> regs = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
                                        ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  LogLine(FormatImportCall(name, regs));
}

void SetTrace(bool on) { g_trace.store(on, std::memory_order_relaxed); }
bool Trace() { return g_trace.load(std::memory_order_relaxed); }

}  // namespace kernel::stats
