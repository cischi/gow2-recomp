// Executable module (LDR_DATA_TABLE_ENTRY, XEX header in guest memory) and
// the exported kernel variables the game imports.
//
// Xenia references (https://github.com/xenia-project/xenia @ 95a5c3ee, BSD):
//  - src/xenia/kernel/xmodule.{h,cc} (X_LDR_DATA_TABLE_ENTRY, hmodule) and
//    user_module.cc (UserModule::LoadFromMemory: copy of the XEX header into
//    guest memory, LDR fields; UserModule::GetSection);
//  - src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc (constructor of
//    XboxkrnlModule: XboxHardwareInfo, XboxKrnlVersion, ExLoadedCommandLine,
//    KeTimeStampBundle, KeDebugMonitorData, KeCertMonitorData) and
//    kernel_state.cc (SetExecutableModule: XexExecutableModuleHandle);
//  - xboxkrnl_modules.cc (XexGetModuleHandle, XexGetModuleSection),
//    xboxkrnl_rtl.cc (RtlImageXexHeaderField), xboxkrnl_misc.cc
//    (KeEnableFpuExceptions).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct ImportRecord;
struct PPCContext;

namespace kernel::module {

// Offsets in X_LDR_DATA_TABLE_ENTRY (0x64 bytes).
inline constexpr uint32_t kLdrSize = 0x64;
inline constexpr uint32_t kLdrDllBase = 0x18;
inline constexpr uint32_t kLdrImageBase = 0x1C;
inline constexpr uint32_t kLdrImageSize = 0x20;
inline constexpr uint32_t kLdrFullImageSize = 0x38;
inline constexpr uint32_t kLdrEntryPoint = 0x3C;
inline constexpr uint32_t kLdrChecksum = 0x48;
inline constexpr uint32_t kLdrXexHeaderBase = 0x58;

struct ExecutableInfo {
  uint32_t image_base = 0, image_size = 0, entry = 0;
  std::vector<uint8_t> header;  // the first Xex2Header::headerSize bytes of the XEX
  std::string name = "default.xex";
};

// Copies the header into the process heap and prepares the executable
// module's LDR (Xenia UserModule::LoadFromMemory). Must be called after
// InitMemory. Returns the hmodule (guest address of the LDR), 0 if memory is
// missing or the header is empty. A new call replaces the module.
uint32_t InitExecutable(const ExecutableInfo&);
uint32_t ExecutableHandle();  // hmodule, 0 before InitExecutable
uint32_t ExecutableHeader();  // guest address of the XEX header
uint32_t ExecutableTitleId();  // title id of XEX_HEADER_EXECUTION_INFO (0x40006), 0 without one

// Fills the areas of the imported variables (type 0 records resolved by
// ResolveImports) like Xenia: XboxHardwareInfo (flags 0x20, 6 CPUs),
// XboxKrnlVersion 2.0xFFFF.0xFFFF.0x80, ExLoadedCommandLine "\"default.xex\"",
// KeDebugMonitorData and KeCertMonitorData 0, XexExecutableModuleHandle =
// hmodule, KeTimeStampBundle with the uptime milliseconds at +16 updated
// every millisecond by a host thread. Must be called after InitExecutable.
// Returns the names of the initialized variables.
std::vector<std::string> InitKernelVariables(std::span<const ImportRecord> records);
// Stops updating KeTimeStampBundle (idempotent).
void StopKernelVariables();

// Kernel modules (xam.xex, xboxkrnl.exe) answer XexGetModuleHandle and
// XexLoadImage with an LDR of their own; XexGetProcedureAddress on them gives
// the KERNEL_EXPORT implementation of the ordinal at a guest address the
// installer makes callable (the runtime owns the function table). The
// installer returns 0 when it has no slot left; without one every kernel
// procedure is not found.
using HostFunction = void(PPCContext&, uint8_t*);
using ProcedureInstaller = uint32_t (*)(HostFunction*);
void SetProcedureInstaller(ProcedureInstaller);

}  // namespace kernel::module
