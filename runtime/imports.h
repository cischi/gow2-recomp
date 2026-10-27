// Kernel/XAM imports: records read by LoadImage, linking of the slots
// in guest memory and the call log format.
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "guest_memory.h"

// An import record as it is in the XEX: `type` 0 = slot of a variable
// (or of a function's address), 1 = function thunk.
struct ImportRecord {
  std::string module;  // "xboxkrnl.exe" / "xam.xex"
  uint32_t thunk;      // guest address of the record
  uint32_t ordinal;
  uint32_t type;
};

// Export name (XenonUtils tables) or "ordinal_0xNNNN".
std::string ImportName(const ImportRecord&);

// Zeroed bytes allocated for each imported variable (the real sizes are not
// known here; the correct content is the job of the kernel variable setup).
inline constexpr uint32_t kVariableImportSize = 0x1000;

struct ImportStats {
  uint32_t function_thunks;  // type 1 records (the recompiled code calls __imp__Name)
  uint32_t function_slots;   // type 0 records of a function: they receive the thunk's address
  uint32_t variables;        // type 0 records without a thunk: they receive a zeroed guest area
};

// Prepares the type 0 slots as the kernel's loader would:
// - if the same module/ordinal also has a thunk (type 1), the slot receives
//   the thunk's guest address (big-endian), which the function table maps to the
//   __imp__Name stub;
// - otherwise it is an exported variable: it receives the address of
//   kVariableImportSize zeroed bytes taken with mem.Alloc.
// Thunks (type 1) are not touched. Every variable is written to the log.
// Error (with a message) for an unknown type, a record outside the
// guest space or a failed allocation.
std::expected<ImportStats, std::string> ResolveImports(GuestMemory& mem, std::span<const ImportRecord> records);

// "[kernel] Name(r3=0x…, r4=0x…, …, r10=0x…)", registers in hexadecimal.
std::string FormatImportCall(std::string_view name, std::span<const uint64_t, 8> r3_to_r10);
