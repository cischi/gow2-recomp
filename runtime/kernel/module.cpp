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
#include <string_view>
#include <thread>

#include "imports.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/registry.h"
#include "log.h"
#include "platform/platform.h"

namespace kernel::module {
namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorNotFound = 0x490;  // X_ERROR_NOT_FOUND
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusNotFound = 0xC0000225;

std::mutex g_mutex;
uint32_t g_hmodule = 0;
uint32_t g_header = 0;
uint32_t g_header_size = 0;
std::string g_name;

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
// module. Only the game's XEX has an LDR: for other names (kernel
// modules) hmodule 0 and X_ERROR_NOT_FOUND, logged.
uint32_t XexGetModuleHandleImpl(const char* name, be<uint32_t>* out) {
  uint32_t hmodule = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!name || EqualNoCase(name, g_name) || EqualNoCase(name, "default")) hmodule = g_hmodule;
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

// Xenia: RtlImageXexHeaderField_entry (xboxkrnl_rtl.cc).
uint32_t RtlImageXexHeaderFieldImpl(uint32_t header, uint32_t key) { return OptHeaderField(header, key); }

// Xenia: KeEnableFpuExceptions_entry (xboxkrnl_misc.cc): no effect (FPU
// exceptions are not emulated).
void KeEnableFpuExceptionsImpl(uint32_t) {}

}  // namespace
}  // namespace kernel::module

KERNEL_EXPORT(XexGetModuleHandle, kernel::module::XexGetModuleHandleImpl)
KERNEL_EXPORT(XexGetModuleSection, kernel::module::XexGetModuleSectionImpl)
KERNEL_EXPORT(RtlImageXexHeaderField, kernel::module::RtlImageXexHeaderFieldImpl)
KERNEL_EXPORT(KeEnableFpuExceptions, kernel::module::KeEnableFpuExceptionsImpl)
