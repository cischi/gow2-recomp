// Executable module and kernel variables (module.h). Xenia references
// @ 95a5c3ee (BSD) cited per function.
#include "kernel/module.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <map>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "imports.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/registry.h"
#include "log.h"
#include "platform/platform.h"
#include "xex_check.h"

namespace kernel::module {
namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorNotFound = 0x490;  // X_ERROR_NOT_FOUND
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusNotFound = 0xC0000225;
constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
constexpr uint32_t kStatusEntrypointNotFound = 0xC0000263;  // X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND

std::mutex g_mutex;
uint32_t g_hmodule = 0;
uint32_t g_header = 0;
uint32_t g_header_size = 0;
std::string g_name;

// Kernel modules: lowercase name -> LDR (zeroed, allocated on first use).
std::map<std::string, uint32_t> g_kernel_modules;
ProcedureInstaller g_installer = nullptr;
std::unordered_map<HostFunction*, uint32_t> g_procedures;  // installed exports -> guest address

std::atomic<bool> g_stamp_run{false};
std::thread g_stamp_thread;

be<uint32_t>* P32(uint32_t a) { return GuestPtr<be<uint32_t>>(a); }

std::string Hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

bool EqualNoCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

std::string Lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

// Xenia: KernelModule (kernel_module.cc), one per built-in module. hmodule of
// the kernel module `name`, 0 if the name is not one. Called with g_mutex held.
uint32_t KernelModuleLocked(const char* name) {
  if (!name) return 0;
  std::string key = Lower(name);
  if (key == "xam") key = "xam.xex";
  if (key == "xboxkrnl") key = "xboxkrnl.exe";
  if (key != "xam.xex" && key != "xboxkrnl.exe") return 0;
  uint32_t& ldr = g_kernel_modules[key];
  if (!ldr) {
    if (MemorySystem* m = Memory()) ldr = m->heap.Alloc(kLdrSize);
    if (ldr) std::memset(GuestPtr<uint8_t>(ldr), 0, kLdrSize);
  }
  return ldr;
}

// Name of the kernel module with this hmodule, empty if none. g_mutex held.
std::string KernelModuleNameLocked(uint32_t hmodule) {
  for (const auto& [name, ldr] : g_kernel_modules)
    if (hmodule && ldr == hmodule) return name;
  return {};
}

// Xenia: XexModule::GetOptHeader(header, key, out) (cpu/xex_module.cc) on a
// header in guest memory: key with low byte 0x00 -> inline value,
// 0x01 -> address of the value, otherwise -> header + offset. 0 if absent.
uint32_t OptHeaderField(uint32_t header, uint32_t key) {
  if (!header) return 0;
  const uint32_t count = *P32(header + 0x14);
  for (uint32_t i = 0; i < count && i < 0x1000; ++i) {
    const uint32_t entry = header + 0x18 + i * 8;
    if (*P32(entry) != key) continue;
    switch (key & 0xFF) {
      case 0x00: return *P32(entry + 4);
      case 0x01: return entry + 4;
      default: return header + *P32(entry + 4);
    }
  }
  return 0;
}

}  // namespace

// Xenia: XModule::XModule (LDR in SystemHeapAlloc) + UserModule::LoadFromMemory.
uint32_t InitExecutable(const ExecutableInfo& info) {
  MemorySystem* m = Memory();
  if (!m || info.header.size() < 0x18) return 0;
  const uint32_t header = m->heap.Alloc(uint32_t(info.header.size()));
  const uint32_t ldr = m->heap.Alloc(kLdrSize);
  if (!header || !ldr) return 0;
  std::memcpy(GuestPtr<uint8_t>(header), info.header.data(), info.header.size());
  std::memset(GuestPtr<uint8_t>(ldr), 0, kLdrSize);
  *P32(ldr + kLdrDllBase) = 0;  // "GetProcAddress will read this"
  *P32(ldr + kLdrImageBase) = info.image_base;
  *P32(ldr + kLdrImageSize) = info.image_size;
  *P32(ldr + kLdrFullImageSize) = info.image_size;
  *P32(ldr + kLdrEntryPoint) = info.entry;
  *P32(ldr + kLdrXexHeaderBase) = header;
  // Xenia puts the module handle in the checksum; here the hmodule already
  // identifies the module (XexGetModuleSection compares it with ExecutableHandle).
  std::lock_guard lock(g_mutex);
  g_hmodule = ldr;
  g_header = header;
  g_header_size = uint32_t(info.header.size());
  g_name = info.name;
  return ldr;
}

uint32_t ExecutableHandle() {
  std::lock_guard lock(g_mutex);
  return g_hmodule;
}
uint32_t ExecutableHeader() {
  std::lock_guard lock(g_mutex);
  return g_header;
}

// Execution info: media id, version, base version, title id (+12), ...
uint32_t ExecutableTitleId() {
  const uint32_t info = OptHeaderField(ExecutableHeader(), 0x40006);
  return info ? uint32_t(*P32(info + 12)) : 0;
}

// Xenia: XboxkrnlModule::XboxkrnlModule and KernelState::SetExecutableModule.
std::vector<std::string> InitKernelVariables(std::span<const ImportRecord> records) {
  std::vector<std::string> done;
  for (const ImportRecord& r : records) {
    if (r.type != 0 || r.module != "xboxkrnl.exe") continue;
    const std::string name = ImportName(r);
    const uint32_t area = *P32(r.thunk);
    if (!area) continue;
    uint8_t* p = GuestPtr<uint8_t>(area);
    if (name == "XboxHardwareInfo") {
      std::memset(p, 0, 16);
      *P32(area) = 0x20;  // flags: bit 5, HDD present (after storage init)
      p[4] = 6;           // number of CPUs
    } else if (name == "XboxKrnlVersion") {
      *GuestPtr<be<uint16_t>>(area) = 2;
      *GuestPtr<be<uint16_t>>(area + 2) = 0xFFFF;
      *GuestPtr<be<uint16_t>>(area + 4) = 0xFFFF;
      p[6] = 0x80;
      p[7] = 0;
    } else if (name == "ExLoadedCommandLine") {
      static constexpr char kCommandLine[] = "\"default.xex\"";
      std::memset(p, 0, 1024);
      std::memcpy(p, kCommandLine, sizeof(kCommandLine));
    } else if (name == "KeDebugMonitorData" || name == "KeCertMonitorData") {
      *P32(area) = 0;  // no debugger / certificate monitor
    } else if (name == "XexExecutableModuleHandle") {
      *P32(area) = ExecutableHandle();
    } else if (name == "KeTimeStampBundle") {
      // { interrupt_time u64, system_time u64, tick_count u32, padding u32 }:
      // Xenia only updates tick_count (uptime in ms) with a 1 ms timer.
      std::memset(p, 0, 24);
      StopKernelVariables();
      g_stamp_run = true;
      g_stamp_thread = std::thread([area] {
        const auto start = std::chrono::steady_clock::now();
        while (g_stamp_run.load(std::memory_order_relaxed)) {
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
          *P32(area + 16) = uint32_t(ms);
          plat::SleepPrecise(1000);  // 1 ms timer like Xenia
        }
      });
    } else {
      continue;
    }
    done.push_back(name);
  }
  return done;
}

void StopKernelVariables() {
  g_stamp_run = false;
  if (g_stamp_thread.joinable()) g_stamp_thread.join();
}

namespace {

// Xenia: XexGetModuleHandle_entry (xboxkrnl_modules.cc): null name = executable
// module; xam.xex and xboxkrnl.exe are the kernel modules; other names
// hmodule 0 and X_ERROR_NOT_FOUND, logged.
uint32_t XexGetModuleHandleImpl(const char* name, be<uint32_t>* out) {
  uint32_t hmodule = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!name || EqualNoCase(name, g_name) || EqualNoCase(name, "default")) hmodule = g_hmodule;
    if (!hmodule) hmodule = KernelModuleLocked(name);
  }
  if (out) *out = hmodule;
  if (!hmodule) {
    LogLine(std::string("[kernel] XexGetModuleHandle: module \"") + (name ? name : "(null)") + "\" not found");
    return kErrorNotFound;
  }
  return kErrorSuccess;
}

// Xenia: XexGetModuleSection_entry -> UserModule::GetSection: looks for the
// resource `name` (8 characters) in the XEX_HEADER_RESOURCE_INFO header
// (0x2FF: size u32, then entries { name[8], address u32, size u32 }).
uint32_t XexGetModuleSectionImpl(uint32_t hmodule, const char* name, be<uint32_t>* data, be<uint32_t>* size) {
  uint32_t header;
  {
    std::lock_guard lock(g_mutex);
    if (!hmodule || hmodule != g_hmodule) {
      LogLine("[kernel] XexGetModuleSection: unknown hmodule " + Hex(hmodule) + " (caller " +
              Hex(ExportCaller()) + ")");
      return kStatusInvalidHandle;
    }
    header = g_header;
  }
  const uint32_t info = OptHeaderField(header, 0x2FF);
  if (info && name) {
    const uint32_t count = (*P32(info) - 4) / 16;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t res = info + 4 + i * 16;
      const char* res_name = GuestPtr<const char>(res);
      if (std::string_view(name) != std::string_view(res_name, strnlen(res_name, 8))) continue;
      if (data) *data = *P32(res + 8);
      if (size) *size = *P32(res + 12);
      return 0;
    }
  }
  LogLine(std::string("[kernel] XexGetModuleSection: section \"") + (name ? name : "(null)") + "\" not found");
  return kStatusNotFound;
}

// Xenia: XexLoadImage_entry (xboxkrnl_modules.cc). Loading the executable or a
// kernel module returns its hmodule; any other XEX is reported missing, so the
// title skips what it wanted from it.
// shortcut: no second XEX is loaded; upgrade when a title ships one it needs.
uint32_t XexLoadImageImpl(const char* name, uint32_t, uint32_t, be<uint32_t>* out) {
  uint32_t hmodule = 0;
  {
    std::lock_guard lock(g_mutex);
    if (name && (EqualNoCase(name, g_name) || EqualNoCase(name, "default.xex"))) hmodule = g_hmodule;
    if (!hmodule) hmodule = KernelModuleLocked(name);
  }
  if (out) *out = hmodule;
  if (!hmodule) {
    LogLine(std::string("[kernel] XexLoadImage: module \"") + (name ? name : "(null)") + "\" not loadable");
    return kStatusObjectNameNotFound;
  }
  return kErrorSuccess;
}

// Xenia: XexUnloadImage_entry. The executable stays loaded.
uint32_t XexUnloadImageImpl(uint32_t hmodule) {
  std::lock_guard lock(g_mutex);
  const bool known = hmodule && (hmodule == g_hmodule || !KernelModuleNameLocked(hmodule).empty());
  return known ? kErrorSuccess : kStatusInvalidHandle;
}

// Xenia: XexGetProcedureAddress_entry. hmodule 0 = executable. On a kernel
// module the ordinal (or, from 0x10000, the address of a name) selects a
// KERNEL_EXPORT, installed once at a guest address by the runtime's installer.
// shortcut: the executable's export table is not read (titles do not export);
// every ordinal or name of the executable is not found.
uint32_t XexGetProcedureAddressImpl(uint32_t hmodule, uint32_t ordinal, be<uint32_t>* out) {
  if (out) *out = 0;
  std::lock_guard lock(g_mutex);
  const std::string module = KernelModuleNameLocked(hmodule);
  if (hmodule && hmodule != g_hmodule && module.empty()) {
    LogLine("[kernel] XexGetProcedureAddress: unknown hmodule " + Hex(hmodule));
    return kStatusInvalidHandle;
  }
  if (module.empty()) {
    LogLine("[kernel] XexGetProcedureAddress: " + Hex(ordinal) + " not exported by the executable");
    return kStatusEntrypointNotFound;
  }
  const char* by_name = ordinal >= 0x10000 ? GuestPtr<const char>(ordinal) : nullptr;
  const std::string name = by_name ? std::string(by_name, strnlen(by_name, 256))  // export names are short
                                   : xexcheck::ExportName(module, ordinal);
  HostFunction* func = FindExport(name);
  uint32_t address = 0;
  if (func) {
    uint32_t& slot = g_procedures[func];
    if (!slot && g_installer) slot = g_installer(func);
    address = slot;
  }
  static std::set<std::string> logged;  // titles look some exports up every frame
  const bool first = logged.insert(module + " " + name).second;
  if (!address) {
    if (first)
      LogLine("[kernel] XexGetProcedureAddress: " + module + " " + name + " not " +
              (func ? "installable" : "implemented"));
    return kStatusEntrypointNotFound;
  }
  if (out) *out = address;
  if (first) LogLine("[kernel] XexGetProcedureAddress: " + module + " " + name + " -> " + Hex(address));
  return kErrorSuccess;
}

// Xenia: RtlImageXexHeaderField_entry (xboxkrnl_rtl.cc).
uint32_t RtlImageXexHeaderFieldImpl(uint32_t header, uint32_t key) { return OptHeaderField(header, key); }

// Xenia: XamGetExecutionId_entry (xam_info.cc): address of the executable's
// XEX_HEADER_EXECUTION_INFO (0x40006: media id, version, title id...).
uint32_t XamGetExecutionIdImpl(be<uint32_t>* info) {
  const uint32_t field = OptHeaderField(ExecutableHeader(), 0x40006);
  if (!field) return kStatusNotFound;
  if (info) *info = field;
  return 0;
}

// Xenia: KeEnableFpuExceptions_entry (xboxkrnl_misc.cc): no effect (FPU
// exceptions are not emulated).
void KeEnableFpuExceptionsImpl(uint32_t) {}

}  // namespace

void SetProcedureInstaller(ProcedureInstaller installer) {
  std::lock_guard lock(g_mutex);
  g_installer = installer;
  g_procedures.clear();
}

}  // namespace kernel::module

namespace kernel {
namespace {
std::unordered_map<std::string_view, PPCFunc*>& Exports() {
  static std::unordered_map<std::string_view, PPCFunc*> exports;  // names are string literals
  return exports;
}
}  // namespace

bool RegisterExport(const char* name, PPCFunc* func) {
  Exports()[name] = func;
  return true;
}

PPCFunc* FindExport(std::string_view name) {
  const auto it = Exports().find(name);
  return it == Exports().end() ? nullptr : it->second;
}
}  // namespace kernel

KERNEL_EXPORT(XexGetModuleHandle, kernel::module::XexGetModuleHandleImpl)
KERNEL_EXPORT(XexGetModuleSection, kernel::module::XexGetModuleSectionImpl)
KERNEL_EXPORT(XexLoadImage, kernel::module::XexLoadImageImpl)
KERNEL_EXPORT(XexUnloadImage, kernel::module::XexUnloadImageImpl)
KERNEL_EXPORT(XexGetProcedureAddress, kernel::module::XexGetProcedureAddressImpl)
KERNEL_EXPORT(RtlImageXexHeaderField, kernel::module::RtlImageXexHeaderFieldImpl)
KERNEL_EXPORT(KeEnableFpuExceptions, kernel::module::KeEnableFpuExceptionsImpl)
KERNEL_EXPORT(XamGetExecutionId, kernel::module::XamGetExecutionIdImpl)
