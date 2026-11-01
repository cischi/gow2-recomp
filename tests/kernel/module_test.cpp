// Executable module and kernel variables (runtime/kernel/module.*): LDR with
// the XEX header in guest memory, XexGetModuleHandle/XexGetModuleSection,
// RtlImageXexHeaderField, imported variables (XboxHardwareInfo,
// KeTimeStampBundle...). Synthetic XEX header; no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "imports.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/module.h"

PPC_EXTERN_FUNC(__imp__XexGetModuleHandle);
PPC_EXTERN_FUNC(__imp__XexGetModuleSection);
PPC_EXTERN_FUNC(__imp__XexLoadImage);
PPC_EXTERN_FUNC(__imp__XexUnloadImage);
PPC_EXTERN_FUNC(__imp__XexGetProcedureAddress);
PPC_EXTERN_FUNC(__imp__RtlImageXexHeaderField);
PPC_EXTERN_FUNC(__imp__KeEnableFpuExceptions);
PPC_EXTERN_FUNC(__imp__XamGetExecutionId);

namespace {

using namespace kernel;
using namespace std::chrono_literals;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
uint16_t Get16(uint32_t a) { return *GuestPtr<be<uint16_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

void PutBe32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = uint8_t(x >> 24);
  v[at + 1] = uint8_t(x >> 16);
  v[at + 2] = uint8_t(x >> 8);
  v[at + 3] = uint8_t(x);
}

// Synthetic XEX2 header with 3 optional headers: inline value (0x30000),
// value address (0x10001) and resources (0x2FF, at +0x40, two entries).
std::vector<uint8_t> MakeHeader() {
  std::vector<uint8_t> h(0x100, 0);
  std::memcpy(h.data(), "XEX2", 4);
  PutBe32(h, 0x08, 0x100);  // headerSize
  PutBe32(h, 0x14, 3);      // headerCount
  PutBe32(h, 0x18, 0x00030000);
  PutBe32(h, 0x1C, 0x400);
  PutBe32(h, 0x20, 0x00010001);
  PutBe32(h, 0x24, 0x82000000);
  PutBe32(h, 0x28, 0x000002FF);
  PutBe32(h, 0x2C, 0x40);
  PutBe32(h, 0x40, 4 + 2 * 16);  // size of the resources header
  std::memcpy(h.data() + 0x44, "GearGame", 8);
  PutBe32(h, 0x4C, 0x82D00000);
  PutBe32(h, 0x50, 0x1234);
  std::memcpy(h.data() + 0x54, "XLAST", 5);
  PutBe32(h, 0x5C, 0x82E00000);
  PutBe32(h, 0x60, 0x80);
  return h;
}

struct Env {
  GuestMemory mem;
  uint32_t scratch = 0;
  uint32_t hmodule = 0;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    scratch = Memory()->heap.Alloc(0x1000);
    REQUIRE(scratch != 0);
    hmodule = module::InitExecutable({0x82000000, 0x1230000, 0x82AAC8D8, MakeHeader(), "default.xex"});
    REQUIRE(hmodule != 0);
  }
};
Env& E() {
  static Env env;
  return env;
}

}  // namespace

TEST_CASE("module: the executable module's LDR with the XEX header copied") {
  Env& e = E();
  const uint32_t ldr = e.hmodule;
  CHECK(module::ExecutableHandle() == ldr);
  CHECK(Get32(ldr + module::kLdrDllBase) == 0);
  CHECK(Get32(ldr + module::kLdrImageBase) == 0x82000000);
  CHECK(Get32(ldr + module::kLdrFullImageSize) == 0x1230000);
  CHECK(Get32(ldr + module::kLdrEntryPoint) == 0x82AAC8D8);
  const uint32_t header = Get32(ldr + module::kLdrXexHeaderBase);
  CHECK(header == module::ExecutableHeader());
  CHECK(std::memcmp(GuestPtr<char>(header), "XEX2", 4) == 0);
}

TEST_CASE("module: XexGetModuleHandle, null name or default.xex; others not found") {
  Env& e = E();
  const uint32_t out = e.scratch, name = e.scratch + 0x100;
  Put32(out, 0xFFFFFFFF);
  CHECK(Call(__imp__XexGetModuleHandle, {0, out}) == 0);
  CHECK(Get32(out) == e.hmodule);
  std::strcpy(GuestPtr<char>(name), "DEFAULT.XEX");
  Put32(out, 0);
  CHECK(Call(__imp__XexGetModuleHandle, {name, out}) == 0);
  CHECK(Get32(out) == e.hmodule);
  std::strcpy(GuestPtr<char>(name), "game.xex");
  CHECK(Call(__imp__XexGetModuleHandle, {name, out}) == 0x490);
  CHECK(Get32(out) == 0);
}

TEST_CASE("module: kernel modules have a handle; XexGetProcedureAddress installs their exports") {
  Env& e = E();
  const uint32_t out = e.scratch + 0x400, name = e.scratch + 0x410, proc_name = e.scratch + 0x440;
  std::strcpy(GuestPtr<char>(name), "XAM.XEX");
  CHECK(Call(__imp__XexGetModuleHandle, {name, out}) == 0);
  const uint32_t xam = Get32(out);
  CHECK(xam != 0);
  CHECK(xam != e.hmodule);
  std::strcpy(GuestPtr<char>(name), "xam.xex");
  Put32(out, 0);
  CHECK(Call(__imp__XexLoadImage, {name, 9, 0, out}) == 0);
  CHECK(Get32(out) == xam);
  CHECK(Call(__imp__XexUnloadImage, {xam}) == 0);

  // Without an installer every kernel procedure is not found.
  module::SetProcedureInstaller(nullptr);
  CHECK(Call(__imp__XexGetProcedureAddress, {xam, 0x279, out}) == 0xC0000263);
  static uint32_t installed = 0;
  static module::HostFunction* last = nullptr;
  module::SetProcedureInstaller([](module::HostFunction* f) {
    last = f;
    return 0x83000000u + 4 * installed++;
  });
  // 0x279 = XamContentAggregateCreateEnumerator (implemented), once per export.
  CHECK(Call(__imp__XexGetProcedureAddress, {xam, 0x279, out}) == 0);
  CHECK(Get32(out) == 0x83000000);
  CHECK(last == FindExport("XamContentAggregateCreateEnumerator"));
  CHECK(Call(__imp__XexGetProcedureAddress, {xam, 0x279, out}) == 0);
  CHECK(Get32(out) == 0x83000000);
  CHECK(installed == 1);
  // By name (an address from 0x10000).
  std::strcpy(GuestPtr<char>(proc_name), "XamContentAggregateCreateEnumerator");
  CHECK(Call(__imp__XexGetProcedureAddress, {xam, proc_name, out}) == 0);
  CHECK(Get32(out) == 0x83000000);
  // Not implemented here (XamMediaVerificationVerify): not found, out 0.
  std::strcpy(GuestPtr<char>(proc_name), "XamMediaVerificationVerify");
  CHECK(Call(__imp__XexGetProcedureAddress, {xam, proc_name, out}) == 0xC0000263);
  CHECK(Get32(out) == 0);
  module::SetProcedureInstaller(nullptr);
}

TEST_CASE("module: XamGetExecutionId points at the execution info header") {
  Env& e = E();
  const uint32_t out = e.scratch + 0x480;
  CHECK(Call(__imp__XamGetExecutionId, {out}) == 0xC0000225);  // the synthetic header has no 0x40006
  CHECK(Call(__imp__XamGetExecutionId, {0}) == 0xC0000225);
  CHECK(module::ExecutableTitleId() == 0);
}

TEST_CASE("module: ExecutableTitleId reads the title id of the execution info") {
  Env& e = E();
  std::vector<uint8_t> h = MakeHeader();
  PutBe32(h, 0x14, 4);  // a fourth optional header: execution info at +0x80
  PutBe32(h, 0x30, 0x00040006);
  PutBe32(h, 0x34, 0x80);
  PutBe32(h, 0x80 + 12, 0x4D5308AB);
  REQUIRE(module::InitExecutable({0x82000000, 0x1230000, 0x82AAC8D8, h, "default.xex"}) != 0);
  CHECK(module::ExecutableTitleId() == 0x4D5308AB);
  // Back to the shared header for the other test cases.
  e.hmodule = module::InitExecutable({0x82000000, 0x1230000, 0x82AAC8D8, MakeHeader(), "default.xex"});
  REQUIRE(e.hmodule != 0);
  CHECK(module::ExecutableTitleId() == 0);
}

TEST_CASE("module: XexLoadImage only knows the executable; other modules are not found") {
  Env& e = E();
  const uint32_t out = e.scratch + 0x300, name = e.scratch + 0x310;
  std::strcpy(GuestPtr<char>(name), "default.xex");
  Put32(out, 0);
  CHECK(Call(__imp__XexLoadImage, {name, 9, 0, out}) == 0);
  CHECK(Get32(out) == e.hmodule);
  CHECK(Call(__imp__XexUnloadImage, {e.hmodule}) == 0);
  // No second module can be loaded (kernel modules have no LDR here): the
  // game must see a failure and skip the optional imports it wanted.
  std::strcpy(GuestPtr<char>(name), "dlc.xex");
  Put32(out, 0xFFFFFFFF);
  CHECK(Call(__imp__XexLoadImage, {name, 9, 0, out}) == 0xC0000034);
  CHECK(Get32(out) == 0);
  CHECK(Call(__imp__XexUnloadImage, {0x12340000}) == 0xC0000008);
}

TEST_CASE("module: XexGetProcedureAddress, invalid handle or no export") {
  Env& e = E();
  const uint32_t out = e.scratch + 0x320;
  Put32(out, 0xFFFFFFFF);
  CHECK(Call(__imp__XexGetProcedureAddress, {0xFFFFFFFF, 0xAFF, out}) == 0xC0000008);
  CHECK(Call(__imp__XexGetProcedureAddress, {e.hmodule, 1, out}) == 0xC0000263);
  CHECK(Get32(out) == 0);
  CHECK(Call(__imp__XexGetProcedureAddress, {0, 1, out}) == 0xC0000263);
}

TEST_CASE("module: XexGetModuleSection looks up the header's resources") {
  Env& e = E();
  const uint32_t data = e.scratch + 0x200, size = e.scratch + 0x204, name = e.scratch + 0x210;
  std::strcpy(GuestPtr<char>(name), "XLAST");
  CHECK(Call(__imp__XexGetModuleSection, {e.hmodule, name, data, size}) == 0);
  CHECK(Get32(data) == 0x82E00000);
  CHECK(Get32(size) == 0x80);
  std::strcpy(GuestPtr<char>(name), "GearGame");  // 8 characters, no terminator in the header
  CHECK(Call(__imp__XexGetModuleSection, {e.hmodule, name, data, size}) == 0);
  CHECK(Get32(data) == 0x82D00000);
  CHECK(Get32(size) == 0x1234);
  std::strcpy(GuestPtr<char>(name), "XLA");
  CHECK(Call(__imp__XexGetModuleSection, {e.hmodule, name, data, size}) == 0xC0000225);
  CHECK(Call(__imp__XexGetModuleSection, {0xFFFFFFFF, name, data, size}) == 0xC0000008);
}

TEST_CASE("module: RtlImageXexHeaderField come XexModule::GetOptHeader") {
  E();
  const uint32_t header = module::ExecutableHeader();
  CHECK(Call(__imp__RtlImageXexHeaderField, {header, 0x00030000}) == 0x400);          // inline value
  CHECK(Call(__imp__RtlImageXexHeaderField, {header, 0x00010001}) == header + 0x24);  // address of the value
  CHECK(Call(__imp__RtlImageXexHeaderField, {header, 0x000002FF}) == header + 0x40);  // header + offset
  CHECK(Call(__imp__RtlImageXexHeaderField, {header, 0x00040006}) == 0);              // absent
  CHECK(Call(__imp__RtlImageXexHeaderField, {0, 0x00030000}) == 0);
  Call(__imp__KeEnableFpuExceptions, {1});  // no effect
}

TEST_CASE("module: imported kernel variables") {
  Env& e = E();
  const uint32_t slots = e.mem.Alloc(0x40, 4);
  uint32_t areas[8];
  for (int i = 0; i < 8; ++i) {
    areas[i] = e.mem.Alloc(0x1000, 0x10);
    std::memset(GuestPtr<uint8_t>(areas[i]), 0xCC, 0x1000);
    Put32(slots + i * 4, areas[i]);
  }
  const std::vector<ImportRecord> records = {
      {"xboxkrnl.exe", slots + 0, 0x156, 0},   // XboxHardwareInfo
      {"xboxkrnl.exe", slots + 4, 0x158, 0},   // XboxKrnlVersion
      {"xboxkrnl.exe", slots + 8, 0x1AE, 0},   // ExLoadedCommandLine
      {"xboxkrnl.exe", slots + 12, 0x59, 0},   // KeDebugMonitorData
      {"xboxkrnl.exe", slots + 16, 0x266, 0},  // KeCertMonitorData
      {"xboxkrnl.exe", slots + 20, 0x193, 0},  // XexExecutableModuleHandle
      {"xboxkrnl.exe", slots + 24, 0xAD, 0},   // KeTimeStampBundle
      {"xboxkrnl.exe", slots + 28, 0x156, 1},  // thunk (type 1): ignored
  };
  const auto names = module::InitKernelVariables(records);
  CHECK(names.size() == 7);
  CHECK(Get32(areas[0]) == 0x20);
  CHECK(GuestPtr<uint8_t>(areas[0])[4] == 6);
  CHECK(Get16(areas[1]) == 2);
  CHECK(Get16(areas[1] + 2) == 0xFFFF);
  CHECK(GuestPtr<uint8_t>(areas[1])[6] == 0x80);
  CHECK(std::string(GuestPtr<char>(areas[2])) == "\"default.xex\"");
  CHECK(Get32(areas[3]) == 0);
  CHECK(Get32(areas[4]) == 0);
  CHECK(Get32(areas[5]) == e.hmodule);
  CHECK(Get32(areas[7]) == 0xCCCCCCCC);
  // KeTimeStampBundle: tick_count at +16 advances (uptime ms), the rest is 0
  CHECK(Get32(areas[6]) == 0);
  const uint32_t t0 = Get32(areas[6] + 16);
  std::this_thread::sleep_for(50ms);
  const uint32_t t1 = Get32(areas[6] + 16);
  CHECK(t1 > t0);
  CHECK(t1 - t0 < 5000);
  module::StopKernelVariables();
}
