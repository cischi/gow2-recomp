#include "imports.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#include "log.h"
#include "xex_check.h"

namespace {

constexpr uint32_t kTypeVariable = 0;
constexpr uint32_t kTypeFunction = 1;

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

std::string Hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace

std::string ImportName(const ImportRecord& r) { return xexcheck::ExportName(r.module, r.ordinal); }

std::expected<ImportStats, std::string> ResolveImports(GuestMemory& mem, std::span<const ImportRecord> records) {
  using std::unexpected;
  if (!mem.base) return unexpected("ResolveImports: guest memory not initialized");

  // function thunk per module/ordinal
  std::map<std::pair<std::string, uint32_t>, uint32_t> thunks;
  for (const auto& r : records) {
    if (r.type != kTypeVariable && r.type != kTypeFunction)
      return unexpected("import " + r.module + " " + ImportName(r) + " at " + Hex(r.thunk) + ": unknown type " +
                        std::to_string(r.type));
    if (uint64_t(r.thunk) + 4 > GuestMemory::kSize)
      return unexpected("import " + ImportName(r) + ": record outside the guest space (" + Hex(r.thunk) + ")");
    if (r.type == kTypeFunction) thunks.emplace(std::pair{r.module, r.ordinal}, r.thunk);
  }

  ImportStats stats{};
  for (const auto& r : records) {
    if (r.type == kTypeFunction) {
      ++stats.function_thunks;
      continue;
    }
    if (auto it = thunks.find({r.module, r.ordinal}); it != thunks.end()) {
      StoreBe32(mem.base + r.thunk, it->second);
      ++stats.function_slots;
      continue;
    }
    const uint32_t var = mem.Alloc(kVariableImportSize, 0x10);
    if (!var) return unexpected("import " + ImportName(r) + ": guest space exhausted for the variable");
    std::memset(mem.base + var, 0, kVariableImportSize);
    StoreBe32(mem.base + r.thunk, var);
    ++stats.variables;
    LogLine("[import] variable " + r.module + " " + ImportName(r) + " (slot " + Hex(r.thunk) + ") -> " + Hex(var) +
            ", " + Hex(kVariableImportSize) + " bytes zeroed");
  }
  return stats;
}

std::string FormatImportCall(std::string_view name, std::span<const uint64_t, 8> r) {
  std::string s = "[kernel] ";
  s += name;
  s += '(';
  for (size_t i = 0; i < 8; ++i) {
    if (i) s += ", ";
    s += "r" + std::to_string(i + 3) + "=" + Hex(r[i]);
  }
  s += ')';
  return s;
}
