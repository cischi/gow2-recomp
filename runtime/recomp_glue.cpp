#include "recomp_glue.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <utility>
#include <vector>

#include "crash_map.h"
#include "guest_memory.h"
#include "imports.h"
#include "log.h"
#include "kernel/ppc_layout.h"

// recomp_glue.h includes the generated ppc_config.h first: this is the
// PPCContext of the recompiled code, which must match the one of
// gow2_kernel/gow2_gpu/gow2_gpu_fake (compiled without ppc_config.h).
GOW2_CHECK_PPC_CONTEXT_LAYOUT();

void LogImport(const char* name, const PPCContext& ctx) {
  LogLine(FormatImportCall(name, ImportRegs(ctx)));
}

void CrashMapInit() {
  std::vector<std::pair<uint32_t, uintptr_t>> table;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    table.emplace_back(uint32_t(m->guest), reinterpret_cast<uintptr_t>(m->host));
  // End of the mapped code: last function + largest gap between two
  // consecutive functions (an overestimate of the last one's size).
  std::vector<uintptr_t> hosts;
  hosts.reserve(table.size());
  for (const auto& e : table) hosts.push_back(e.second);
  std::sort(hosts.begin(), hosts.end());
  uintptr_t max_gap = 0;
  for (size_t i = 1; i < hosts.size(); ++i) max_gap = std::max(max_gap, hosts[i] - hosts[i - 1]);
  CrashMapInitFrom(table, hosts.empty() ? 0 : hosts.back() + max_gap);
}

static_assert(kFunctionTableEnd <= GuestMemory::kSize, "function table outside the guest space");

std::expected<FunctionTableStats, std::string> InstallFunctionTable(uint8_t* base) {
  if (!base) return std::unexpected("InstallFunctionTable: guest memory not initialized");
  FunctionTableStats stats{};
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE || (m->guest & 3)) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "function table: guest 0x%llX outside the code, skipped",
                    static_cast<unsigned long long>(m->guest));
      LogLine(buf);
      ++stats.skipped;
      continue;
    }
    PPC_LOOKUP_FUNC(base, m->guest) = m->host;
    ++stats.installed;
  }
  return stats;
}

PPCFunc* LookupFunction(uint8_t* base, uint32_t guest) {
  if (guest < PPC_CODE_BASE || guest >= kExtraFunctionBase + kExtraFunctions * 4 || (guest & 3)) return nullptr;
  return PPC_LOOKUP_FUNC(base, guest);
}

uint32_t InstallExtraFunction(uint8_t* base, PPCFunc* func) {
  static std::atomic<uint32_t> used{0};
  const uint32_t i = used.fetch_add(1);
  if (i >= kExtraFunctions) return 0;
  const uint32_t guest = uint32_t(kExtraFunctionBase + i * 4);
  PPC_LOOKUP_FUNC(base, guest) = func;
  return guest;
}
