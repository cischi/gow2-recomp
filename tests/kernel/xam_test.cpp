// XAM (runtime/kernel/xam_exports.cpp) and service stubs (stubs_exports.cpp):
// local user, contents on user_data/content (creation, write,
// close, reopen, hostile names), enumerator, UIs that signal
// the XOVERLAPPED (event and completion routine), initial notifications,
// language, and the "offline" answers of network, audio, XMA and voice.
// Synthetic guest structures in a real GuestMemory arena: no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "settings.h"
#include "kernel/guest.h"
#include "kernel/io/vfs.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "kernel/xam.h"
#include "kernel/xma.h"
#include "../temp_dir.h"

PPC_EXTERN_FUNC(__imp__XamUserGetSigninState);
PPC_EXTERN_FUNC(__imp__XamUserGetXUID);
PPC_EXTERN_FUNC(__imp__XamUserGetName);
PPC_EXTERN_FUNC(__imp__XamUserGetSigninInfo);
PPC_EXTERN_FUNC(__imp__XamUserCheckPrivilege);
PPC_EXTERN_FUNC(__imp__XamContentCreateEx);
PPC_EXTERN_FUNC(__imp__XamContentClose);
PPC_EXTERN_FUNC(__imp__XamContentDelete);
PPC_EXTERN_FUNC(__imp__XamContentCreateEnumerator);
PPC_EXTERN_FUNC(__imp__XamEnumerate);
PPC_EXTERN_FUNC(__imp__XamContentGetCreator);
PPC_EXTERN_FUNC(__imp__XamContentGetDeviceState);
PPC_EXTERN_FUNC(__imp__XamContentGetDeviceData);
PPC_EXTERN_FUNC(__imp__XamShowSigninUI);
PPC_EXTERN_FUNC(__imp__XamShowDeviceSelectorUI);
PPC_EXTERN_FUNC(__imp__XamShowKeyboardUI);
PPC_EXTERN_FUNC(__imp__XamShowMessageBoxUIEx);
PPC_EXTERN_FUNC(__imp__XamNotifyCreateListener);
PPC_EXTERN_FUNC(__imp__XNotifyGetNext);
PPC_EXTERN_FUNC(__imp__XGetLanguage);
PPC_EXTERN_FUNC(__imp__XamLoaderGetLaunchDataSize);
PPC_EXTERN_FUNC(__imp__XGetVideoMode);
PPC_EXTERN_FUNC(__imp__NetDll_XNetStartup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetRandom);
PPC_EXTERN_FUNC(__imp__NetDll_WSAStartup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetGetTitleXnAddr);
PPC_EXTERN_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus);
PPC_EXTERN_FUNC(__imp__NetDll_socket);
PPC_EXTERN_FUNC(__imp__NetDll_send);
PPC_EXTERN_FUNC(__imp__NetDll_sendto);
PPC_EXTERN_FUNC(__imp__NetDll_closesocket);
PPC_EXTERN_FUNC(__imp__NetDll_shutdown);
PPC_EXTERN_FUNC(__imp__NetDll_setsockopt);
PPC_EXTERN_FUNC(__imp__NetDll_getsockopt);
PPC_EXTERN_FUNC(__imp__NetDll_getsockname);
PPC_EXTERN_FUNC(__imp__NetDll_getpeername);
PPC_EXTERN_FUNC(__imp__NetDll_ioctlsocket);
PPC_EXTERN_FUNC(__imp__NetDll_bind);
PPC_EXTERN_FUNC(__imp__NetDll_connect);
PPC_EXTERN_FUNC(__imp__NetDll_listen);
PPC_EXTERN_FUNC(__imp__NetDll_accept);
PPC_EXTERN_FUNC(__imp__NetDll_select);
PPC_EXTERN_FUNC(__imp__NetDll_recv);
PPC_EXTERN_FUNC(__imp__NetDll_recvfrom);
PPC_EXTERN_FUNC(__imp__NetDll_WSARecv);
PPC_EXTERN_FUNC(__imp__NetDll_WSARecvFrom);
PPC_EXTERN_FUNC(__imp__NetDll_WSASend);
PPC_EXTERN_FUNC(__imp__NetDll_WSASendTo);
PPC_EXTERN_FUNC(__imp__NetDll_WSAEventSelect);
PPC_EXTERN_FUNC(__imp__NetDll_WSACancelOverlappedIO);
PPC_EXTERN_FUNC(__imp__NetDll_WSAGetLastError);
PPC_EXTERN_FUNC(__imp__NetDll_inet_addr);
PPC_EXTERN_FUNC(__imp__NetDll_XNetInAddrToString);
PPC_EXTERN_FUNC(__imp__NetDll_WSACreateEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSASetEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSAResetEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSACloseEvent);
PPC_EXTERN_FUNC(__imp__NetDll_WSAWaitForMultipleEvents);
PPC_EXTERN_FUNC(__imp__NetDll_XNetDnsLookup);
PPC_EXTERN_FUNC(__imp__NetDll_XNetDnsRelease);
PPC_EXTERN_FUNC(__imp__XMACreateContext);
PPC_EXTERN_FUNC(__imp__XMAReleaseContext);
PPC_EXTERN_FUNC(__imp__XamVoiceCreate);
PPC_EXTERN_FUNC(__imp__XMsgInProcessCall);
PPC_EXTERN_FUNC(__imp__XMsgStartIORequest);
PPC_EXTERN_FUNC(__imp__XMsgCancelIORequest);
PPC_EXTERN_FUNC(__imp__XMsgCompleteIORequest);
PPC_EXTERN_FUNC(__imp__XamGetOverlappedResult);
PPC_EXTERN_FUNC(__imp__XamTaskSchedule);
PPC_EXTERN_FUNC(__imp__XamTaskCloseHandle);
PPC_EXTERN_FUNC(__imp__XamContentAggregateCreateEnumerator);
PPC_EXTERN_FUNC(__imp__XamGetPrivateEnumStructureFromHandle);
PPC_EXTERN_FUNC(__imp__XamVoiceHeadsetPresent);
PPC_EXTERN_FUNC(__imp__XamVoiceIsActiveProcess);
PPC_EXTERN_FUNC(__imp__XamUserGetDeviceContext);
PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__KeDelayExecutionThread);

namespace fs = std::filesystem;
using namespace kernel;
using namespace std::chrono_literals;

namespace {

// Codes (Xenia src/xenia/xbox.h).
constexpr uint32_t kErrNoMoreFiles = 0x12, kErrInvalidParameter = 0x57, kErrAlreadyExists = 0xB7,
                   kErrIoPending = 0x3E5, kErrPathNotFound = 3, kErrFileNotFound = 2, kErrDeviceNotConnected = 0x48F,
                   kErrNoSuchUser = 0x525, kErrAccessDenied = 5, kErrAccessDeniedFile = 5;
constexpr uint32_t kEInvalidArg = 0x80070057, kENoSuchUser = 0x80070525;
constexpr uint32_t kXnSysSigninChanged = 0x0000000A, kXnSysStorageDevicesChanged = 0x0000000B;
constexpr uint32_t kXContentDataSize = 0x134;
constexpr uint32_t kGenericRead = 0x80000000, kGenericWrite = 0x40000000, kSynchronize = 0x00100000;
constexpr uint32_t kFileSynchronousIoNonalert = 0x20, kFileNonDirectoryFile = 0x40;
constexpr uint32_t kOpenIf = 3;

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
uint64_t Get64(uint32_t a) { return *GuestPtr<be<uint64_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }
void Put64(uint32_t a, uint64_t v) { *GuestPtr<be<uint64_t>>(a) = v; }

// ---- fake guest functions for the APC -------------------------------------------

constexpr uint32_t kFnUiApc = 0x82000100;
constexpr uint32_t kFnRoutine = 0x82000110;

uint32_t CallRegs(PPCFunc* fn, std::initializer_list<uint64_t> args);

std::atomic<uint32_t> g_routine_args[3];
std::atomic<int> g_routine_count{0};
std::atomic<GuestThread*> g_routine_thread{nullptr};
PPC_FUNC(FnRoutine) {
  g_routine_args[0] = ctx.r3.u32;
  g_routine_args[1] = ctx.r4.u32;
  g_routine_args[2] = ctx.r5.u32;
  g_routine_thread = GuestThread::Current();
  ++g_routine_count;
}

struct UiArgs {
  uint32_t device_out, overlapped, timeout;
};
UiArgs g_ui;
std::atomic<uint32_t> g_ui_status{0}, g_wait_status{0};
PPC_FUNC(FnUiApc) {
  const UiArgs a = g_ui;
  // XamShowDeviceSelectorUI(user, content_type, flags, total_requested(u64), device_id*, overlapped)
  g_ui_status = CallRegs(__imp__XamShowDeviceSelectorUI, {0, 1, 0, 0, a.device_out, a.overlapped});
  g_wait_status = CallRegs(__imp__KeDelayExecutionThread, {1, 1, a.timeout});
  ctx.r3.u64 = 0;
}

// XamTaskSchedule task: completes g_task_overlapped like the XDK's XMsg tasks.
constexpr uint32_t kFnTask = 0x82000120;
std::atomic<uint32_t> g_task_overlapped{0}, g_task_message{0};
PPC_FUNC(FnTask) {
  g_task_message = ctx.r3.u32;
  CallRegs(__imp__XMsgCompleteIORequest, {g_task_overlapped.load(), 0x80070012, 0x65B, 7});
  ctx.r3.u64 = 0;
}

PPCFunc* ResolveFn(uint32_t guest) {
  switch (guest) {
    case kFnTask: return &FnTask;
    case kFnUiApc: return &FnUiApc;
    case kFnRoutine: return &FnRoutine;
    default: return nullptr;
  }
}

// ---- environment --------------------------------------------------------------

struct Env {
  GuestMemory mem;
  gow2_test::TempDir tmp{"gow2_xam_test"};  // unique per process (ctest -j)
  fs::path base, content, outside;
  uint32_t stack = 0;
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    REQUIRE(InitThreads(mem, &ResolveFn, 0x40000));
    REQUIRE(xma::Init(mem));
    base = tmp.path;
    content = base / "user_data" / "content";
    outside = base / "user_data" / "outside";
    fs::create_directories(content);
    fs::create_directories(outside);
    fs::create_directories(base / "game");
    std::ofstream(base / "game" / "keep.txt") << "k";
    io::SetMounts({{"game:", base / "game", false}});
    xam::SetContentRoot(content);
    stack = Alloc(0x400);
  }
  uint32_t Alloc(uint32_t size) {
    const uint32_t a = mem.Alloc(size, 0x10);
    REQUIRE(a != 0);
    std::memset(mem.base + a, 0, size);
    return a;
  }
  uint32_t CStr(std::string_view s) {
    const uint32_t a = Alloc(uint32_t(s.size() + 1));
    std::memcpy(mem.base + a, s.data(), s.size());
    return a;
  }
  // Terminated big-endian UTF-16 string.
  uint32_t WStr(std::u16string_view s) {
    const uint32_t a = Alloc(uint32_t(2 * s.size() + 2));
    for (size_t i = 0; i < s.size(); ++i) *GuestPtr<be<uint16_t>>(a + uint32_t(2 * i)) = uint16_t(s[i]);
    return a;
  }
  // XCONTENT_DATA (Xenia xam/content_manager.h): device_id, content_type,
  // display_name[128] UTF-16, file_name[42], padding[2].
  uint32_t ContentData(std::string_view file_name, uint32_t type = 1, uint32_t device = 1) {
    const uint32_t d = Alloc(kXContentDataSize);
    Put32(d, device);
    Put32(d + 4, type);
    for (size_t i = 0; i < file_name.size() && i < 127; ++i)
      *GuestPtr<be<uint16_t>>(d + 8 + uint32_t(2 * i)) = uint16_t(uint8_t(file_name[i]));
    std::memcpy(mem.base + d + 0x108, file_name.data(), std::min<size_t>(file_name.size(), 41));
    return d;
  }
};

Env& E() {
  static Env env;
  return env;
}

uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  ctx.r1.u64 = E().stack;
  size_t i = 0;
  for (uint64_t a : args) {
    if (i < 8)
      *regs[i] = a;
    else
      Put32(E().stack + 0x54 + uint32_t(i - 8) * 8, uint32_t(a));
    ++i;
  }
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}
uint32_t CallRegs(PPCFunc* fn, std::initializer_list<uint64_t> args) {
  PPCContext ctx{};
  uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                       &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  size_t i = 0;
  for (uint64_t a : args) *regs[i++] = a;
  ctx.lr = 0x82001234;
  fn(ctx, g_base);
  return ctx.r3.u32;
}

// XOVERLAPPED (Xenia XAM_OVERLAPPED): result, length, context, event,
// completion_routine, completion_context, extended_error.
uint32_t NewOverlapped(uint32_t event = 0, uint32_t routine = 0) {
  const uint32_t o = E().Alloc(0x1C);
  Put32(o, 0xDEAD);
  Put32(o + 0x0C, event);
  Put32(o + 0x10, routine);
  return o;
}
uint32_t NewEvent() {
  Env& e = E();
  const uint32_t ev = e.Alloc(4);
  // NtCreateEvent is not hooked up here: real events via the handle table.
  Put32(ev, HandleCreate(std::make_shared<Event>(true, false)));
  return Get32(ev);
}
bool Signaled(uint32_t handle) {
  auto obj = HandleGet(handle);
  REQUIRE(obj);
  const std::shared_ptr<KernelObject> one[1] = {obj};
  const int64_t zero = 0;
  return WaitAny(one, &zero, false) == kStatusWait0;
}

// Opens a package with XamContentCreateEx; returns the status and the disposition.
struct Pkg {
  uint32_t status, disposition;
};
Pkg OpenPackage(std::string_view root, std::string_view name, uint32_t flags, uint32_t type = 1) {
  Env& e = E();
  const uint32_t disp = e.Alloc(4);
  Put32(disp, 0xFFFF);
  const uint32_t st = Call(__imp__XamContentCreateEx,
                           {0, e.CStr(root), e.ContentData(name, type), flags, disp, 0, 0, 0, 0});
  return {st, Get32(disp)};
}

// Writes "data" to <device>\<name> with NtCreateFile/NtWriteFile.
bool WriteGuestFile(std::string_view path, std::string_view data) {
  Env& e = E();
  const uint32_t out = e.Alloc(4), iosb = e.Alloc(8);
  const uint32_t oa = e.Alloc(12);
  const uint32_t str = e.Alloc(8), buf = e.CStr(path);
  *GuestPtr<be<uint16_t>>(str) = uint16_t(path.size());
  *GuestPtr<be<uint16_t>>(str + 2) = uint16_t(path.size() + 1);
  Put32(str + 4, buf);
  Put32(oa + 4, str);
  Put32(oa + 8, 0x40);
  const uint32_t st = Call(__imp__NtCreateFile, {out, kGenericWrite | kSynchronize, oa, iosb, 0, 0, 7, kOpenIf,
                                                  kFileSynchronousIoNonalert | kFileNonDirectoryFile});
  if (st != 0) return false;
  const uint32_t src = e.CStr(data);
  const uint32_t w = Call(__imp__NtWriteFile, {Get32(out), 0, 0, 0, iosb, src, uint32_t(data.size()), 0});
  Call(__imp__NtClose, {Get32(out)});
  return w == 0;
}

std::string ReadHost(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

// ---- user -------------------------------------------------------------------

TEST_CASE("xam: sign-in state of the four indices, XUID and name") {
  Env& e = E();
  CHECK(Call(__imp__XamUserGetSigninState, {0}) == 1);
  CHECK(Call(__imp__XamUserGetSigninState, {1}) == 0);
  CHECK(Call(__imp__XamUserGetSigninState, {2}) == 0);
  CHECK(Call(__imp__XamUserGetSigninState, {3}) == 0);
  CHECK(Call(__imp__XamUserGetSigninState, {4}) == 0);

  const uint32_t xuid = e.Alloc(8);
  CHECK(Call(__imp__XamUserGetXUID, {0, 1, xuid}) == 0);
  CHECK(Get64(xuid) == 0xE000000000000001ull);
  CHECK(Call(__imp__XamUserGetXUID, {1, 1, xuid}) == kENoSuchUser);
  CHECK(Get64(xuid) == 0);
  CHECK(Call(__imp__XamUserGetXUID, {4, 1, xuid}) == kEInvalidArg);
  CHECK(Call(__imp__XamUserGetXUID, {0, 1, 0}) == kEInvalidArg);

  const uint32_t name = e.Alloc(32);
  CHECK(Call(__imp__XamUserGetName, {0, name, 32}) == 0);
  CHECK(std::string(GuestPtr<char>(name)) == "Player");
  CHECK(Call(__imp__XamUserGetName, {1, name, 32}) == kENoSuchUser);
  CHECK(Call(__imp__XamUserGetName, {4, name, 32}) == kEInvalidArg);

  const uint32_t info = e.Alloc(40);
  CHECK(Call(__imp__XamUserGetSigninInfo, {0, 0, info}) == 0);
  CHECK(Get64(info) == 0xE000000000000001ull);
  CHECK(Get32(info + 12) == 1);  // signin_state
  CHECK(std::string(GuestPtr<char>(info + 24)) == "Player");
  CHECK(Call(__imp__XamUserGetSigninInfo, {1, 0, info}) == kENoSuchUser);

  const uint32_t priv = e.Alloc(4);
  Put32(priv, 0xFFFF);
  CHECK(Call(__imp__XamUserCheckPrivilege, {0, 0x1, priv}) == 0);
  CHECK(Get32(priv) == 0);  // Xenia: denies everything, games do not try to go online
}

// ---- contents -----------------------------------------------------------------

TEST_CASE("xam: content created, written, closed and reopened") {
  Env& e = E();
  const auto created = OpenPackage("save", "Slot0", 1 /* CREATE_NEW */);
  CHECK(created.status == 0);
  CHECK(created.disposition == 1);  // created
  CHECK(fs::is_directory(e.content / "Slot0"));

  CHECK(WriteGuestFile("save:\\profile.bin", "game-data"));
  CHECK(ReadHost(e.content / "Slot0" / "profile.bin") == "game-data");

  // Same root name already open, and CREATE_NEW on a content that exists.
  CHECK(OpenPackage("save", "Other", 1).status == kErrAlreadyExists);
  CHECK(Call(__imp__XamContentClose, {e.CStr("save"), 0}) == 0);
  CHECK(!io::Resolve("save:\\profile.bin"));  // root unmounted
  CHECK(Call(__imp__XamContentClose, {e.CStr("save"), 0}) == kErrFileNotFound);
  CHECK(OpenPackage("save", "Slot0", 1).status == kErrAlreadyExists);

  const auto reopened = OpenPackage("save", "Slot0", 3 /* OPEN_EXISTING */);
  CHECK(reopened.status == 0);
  CHECK(reopened.disposition == 2);  // opened
  auto host = io::Resolve("save:\\profile.bin");
  REQUIRE(host);
  CHECK(ReadHost(*host) == "game-data");
  CHECK(Call(__imp__XamContentClose, {e.CStr("save"), 0}) == 0);

  // OPEN_EXISTING on a missing content, OPEN_ALWAYS creates it.
  CHECK(OpenPackage("save", "Missing", 3).status == kErrPathNotFound);
  CHECK(!fs::exists(e.content / "Missing"));
  const auto always = OpenPackage("save", "Missing", 4);
  CHECK(always.status == 0);
  CHECK(always.disposition == 1);
  CHECK(Call(__imp__XamContentClose, {e.CStr("save"), 0}) == 0);
  // TRUNCATE_EXISTING deletes and recreates; CREATE_ALWAYS likewise.
  CHECK(OpenPackage("save", "Slot0", 5).disposition == 1);
  CHECK(!fs::exists(e.content / "Slot0" / "profile.bin"));
  CHECK(Call(__imp__XamContentClose, {e.CStr("save"), 0}) == 0);
  CHECK(OpenPackage("save", "Slot0", 2).disposition == 1);
  CHECK(Call(__imp__XamContentClose, {e.CStr("save"), 0}) == 0);
}

TEST_CASE("xam: hostile names do not leave user_data/content and do not steal mounts") {
  Env& e = E();
  const std::set<fs::path> before = [&] {
    std::set<fs::path> s;
    for (auto& p : fs::recursive_directory_iterator(e.base)) s.insert(p.path());
    return s;
  }();

  // The content file name cannot contain paths.
  for (const char* evil : {"..\\..\\outside\\x", "../outside/x", "..", "C:\\Windows", "CON", "a.", "x::$DATA"}) {
    const auto r = OpenPackage("evil", evil, 4);
    CHECK(r.status == 0);
    CHECK(Call(__imp__XamContentClose, {e.CStr("evil"), 0}) == 0);
  }
  // Everything that was created lives under content/.
  for (auto& p : fs::recursive_directory_iterator(e.base)) {
    if (before.count(p.path())) continue;
    const auto rel = p.path().lexically_relative(e.content);
    CHECK((!rel.empty() && *rel.begin() != ".."));
  }
  CHECK(fs::is_directory(e.outside));
  CHECK(fs::directory_iterator(e.outside) == fs::directory_iterator());

  // Roots that coincide with an existing mount, or invalid.
  CHECK(OpenPackage("game", "Slot9", 4).status == kErrAlreadyExists);
  CHECK(io::Resolve("game:\\keep.txt"));
  CHECK(OpenPackage("", "Slot9", 4).status == kErrInvalidParameter);
  CHECK(OpenPackage("a\\b", "Slot9", 4).status == kErrInvalidParameter);
  CHECK(OpenPackage("a:b", "Slot9", 4).status == kErrInvalidParameter);
  CHECK(!fs::exists(e.content / "Slot9"));
  // Unknown flag.
  CHECK(OpenPackage("save", "Slot9", 9).status == kErrInvalidParameter);
}

TEST_CASE("xam: XamContentCreateEx overlapped signals event and disposition") {
  Env& e = E();
  const uint32_t ev = NewEvent();
  const uint32_t ov = NewOverlapped(ev);
  const uint32_t disp = e.Alloc(4);
  CHECK(Call(__imp__XamContentCreateEx, {0, e.CStr("ovl"), e.ContentData("Ovl"), 4, disp, 0, 0, 0, ov}) ==
        kErrIoPending);
  CHECK(Get32(ov) == 0);          // result
  CHECK(Get32(ov + 4) == 1);      // length = disposition (created)
  CHECK(Get32(ov + 0x18) == 0);   // extended error
  CHECK(Get32(disp) == 1);
  CHECK(Signaled(ev));
  CHECK(Call(__imp__XamContentClose, {e.CStr("ovl"), 0}) == 0);

  // Error: the overlapped's result carries the code.
  const uint32_t ov2 = NewOverlapped();
  CHECK(Call(__imp__XamContentCreateEx, {0, e.CStr("ovl"), e.ContentData("DoesNotExist"), 3, 0, 0, 0, 0, ov2}) ==
        kErrIoPending);
  CHECK(Get32(ov2) == kErrPathNotFound);
  CHECK(Get32(ov2 + 0x18) == 0x80070000u + kErrPathNotFound);
  CHECK(Get32(ov2 + 4) == 0);  // unknown disposition
  CHECK(Call(__imp__XamContentClose, {e.CStr("ovl"), 0}) == kErrFileNotFound);
}

TEST_CASE("xam: content enumerator") {
  Env& e = E();
  fs::remove_all(e.content);
  fs::create_directories(e.content);
  for (const char* n : {"Alpha", "Beta", "Gamma"}) {
    CHECK(OpenPackage("en", n, 4).status == 0);
    CHECK(Call(__imp__XamContentClose, {e.CStr("en"), 0}) == 0);
  }
  const uint32_t buf_size = e.Alloc(4), handle = e.Alloc(4);
  // Unknown device.
  CHECK(Call(__imp__XamContentCreateEnumerator, {0, 7, 1, 0, 2, buf_size, handle}) == kEInvalidArg);
  CHECK(Get32(buf_size) == 0);
  // All devices (0) and the hard disk (1): 2 elements per call.
  CHECK(Call(__imp__XamContentCreateEnumerator, {0, 1, 1, 0, 2, buf_size, handle}) == 0);
  CHECK(Get32(buf_size) == 2 * kXContentDataSize);
  const uint32_t h = Get32(handle);
  CHECK(h != 0);
  const uint32_t out = e.Alloc(4 * kXContentDataSize), count = e.Alloc(4);
  std::set<std::string> names;
  CHECK(Call(__imp__XamEnumerate, {h, 0, out, 2 * kXContentDataSize, count, 0}) == 0);
  CHECK(Get32(count) == 2);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t item = out + i * kXContentDataSize;
    CHECK(Get32(item) == 1);  // device id
    names.insert(std::string(GuestPtr<char>(item + 0x108)));
  }
  CHECK(Call(__imp__XamEnumerate, {h, 0, out, 2 * kXContentDataSize, count, 0}) == 0);
  CHECK(Get32(count) == 1);
  names.insert(std::string(GuestPtr<char>(out + 0x108)));
  CHECK(names == std::set<std::string>{"Alpha", "Beta", "Gamma"});
  CHECK(Call(__imp__XamEnumerate, {h, 0, out, 2 * kXContentDataSize, count, 0}) == kErrNoMoreFiles);
  // Neither items_returned nor overlapped, or with a wrong handle.
  CHECK(Call(__imp__XamEnumerate, {h, 0, out, 2 * kXContentDataSize, 0, 0}) == kErrInvalidParameter);
  CHECK(Call(__imp__XamEnumerate, {0x1234, 0, out, 64, count, 0}) == 6 /* ERROR_INVALID_HANDLE */);
  // With overlapped: pending, completes immediately with length = elements.
  CHECK(Call(__imp__XamContentCreateEnumerator, {0, 0, 1, 0, 8, buf_size, handle}) == 0);
  const uint32_t ov = NewOverlapped();
  CHECK(Call(__imp__XamEnumerate, {Get32(handle), 0, out, 4 * kXContentDataSize, 0, ov}) == kErrIoPending);
  CHECK(Get32(ov) == 0);
  CHECK(Get32(ov + 4) == 3);
  // The handle is closed with NtClose.
  CHECK(Call(__imp__NtClose, {Get32(handle)}) == 0);
  CHECK(Call(__imp__NtClose, {h}) == 0);
  CHECK(Call(__imp__NtClose, {h}) != 0);
}

TEST_CASE("xam: creator, device state and data, deletion") {
  Env& e = E();
  CHECK(OpenPackage("cr", "Savegame", 4, 1).status == 0);
  CHECK(Call(__imp__XamContentClose, {e.CStr("cr"), 0}) == 0);

  const uint32_t is_creator = e.Alloc(4), xuid = e.Alloc(8);
  // XamContentGetCreator(user, content_data, is_creator*, creator_xuid*, overlapped)
  CHECK(Call(__imp__XamContentGetCreator, {0, e.ContentData("Savegame", 1), is_creator, xuid, 0}) == 0);
  CHECK(Get32(is_creator) == 1);
  CHECK(Get64(xuid) == 0xE000000000000001ull);
  CHECK(Call(__imp__XamContentGetCreator, {0, e.ContentData("Nobody", 1), is_creator, xuid, 0}) == kErrPathNotFound);

  CHECK(Call(__imp__XamContentGetDeviceState, {1, 0}) == 0);
  CHECK(Call(__imp__XamContentGetDeviceState, {5, 0}) == kErrDeviceNotConnected);
  const uint32_t ov = NewOverlapped();
  CHECK(Call(__imp__XamContentGetDeviceState, {1, ov}) == kErrIoPending);
  CHECK(Get32(ov) == 0);
  const uint32_t ov2 = NewOverlapped();
  CHECK(Call(__imp__XamContentGetDeviceState, {5, ov2}) == kErrIoPending);
  CHECK(Get32(ov2) == 0x65B);                          // ERROR_FUNCTION_FAILED
  CHECK(Get32(ov2 + 0x18) == kErrDeviceNotConnected);  // like Xenia: raw Win32 code
  CHECK(Get32(ov2 + 4) == 0);

  // XamContentGetDeviceData: id 1, "Hard Drive".
  const uint32_t dd = e.Alloc(0x50);
  CHECK(Call(__imp__XamContentGetDeviceData, {1, dd}) == 0);
  CHECK(Get32(dd) == 1);
  CHECK(Get64(dd + 8) > Get64(dd + 16));  // total > free
  std::string name;
  for (int i = 0; i < 28 && *GuestPtr<be<uint16_t>>(dd + 24 + 2 * i); ++i)
    name.push_back(char(uint16_t(*GuestPtr<be<uint16_t>>(dd + 24 + 2 * i))));
  CHECK(name == "Hard Drive");
  CHECK(Call(__imp__XamContentGetDeviceData, {9, dd}) == kErrDeviceNotConnected);

  // XamContentDelete: rejected if open, then deletes.
  CHECK(OpenPackage("cr", "Savegame", 3).status == 0);
  CHECK(Call(__imp__XamContentDelete, {0, e.ContentData("Savegame"), 0}) == kErrAccessDenied);
  CHECK(Call(__imp__XamContentClose, {e.CStr("cr"), 0}) == 0);
  CHECK(Call(__imp__XamContentDelete, {0, e.ContentData("Savegame"), 0}) == 0);
  CHECK(!fs::exists(e.content / "Savegame"));
  CHECK(Call(__imp__XamContentDelete, {0, e.ContentData("Savegame"), 0}) == kErrFileNotFound);
}

// ---- UI -----------------------------------------------------------------------

TEST_CASE("xam: system UIs complete immediately and signal the overlapped") {
  Env& e = E();
  // Device selector: device id 1.
  {
    const uint32_t ev = NewEvent(), ov = NewOverlapped(ev), dev = e.Alloc(4);
    CHECK(Call(__imp__XamShowDeviceSelectorUI, {0, 1, 0, 0, dev, ov}) == kErrIoPending);
    CHECK(Get32(dev) == 1);
    CHECK(Get32(ov) == 0);
    CHECK(Signaled(ev));
    // Without overlapped: direct result.
    Put32(dev, 0);
    CHECK(Call(__imp__XamShowDeviceSelectorUI, {0, 1, 0, 0, dev, 0}) == 0);
    CHECK(Get32(dev) == 1);
  }
  // Keyboard: the buffer receives the default text (UTF-16 BE).
  {
    const uint32_t ev = NewEvent(), ov = NewOverlapped(ev), buf = e.Alloc(64);
    // XamShowKeyboardUI(user, flags, default_text, title, description, buffer, buffer_length, overlapped)
    CHECK(Call(__imp__XamShowKeyboardUI, {0, 0, e.WStr(u"Name"), e.WStr(u"T"), e.WStr(u"D"), buf, 32, ov}) ==
          kErrIoPending);
    CHECK(uint16_t(*GuestPtr<be<uint16_t>>(buf)) == u'N');
    CHECK(uint16_t(*GuestPtr<be<uint16_t>>(buf + 6)) == u'e');
    CHECK(uint16_t(*GuestPtr<be<uint16_t>>(buf + 8)) == 0);
    CHECK(Get32(ov) == 0);
    CHECK(Signaled(ev));
    CHECK(Call(__imp__XamShowKeyboardUI, {0, 0, 0, 0, 0, 0, 32, ov}) == kErrInvalidParameter);
    // buffer_length that exceeds the end of the guest space: rejected
    CHECK(Call(__imp__XamShowKeyboardUI, {0, 0, 0, 0, 0, 0xFFFFFFF0, 0x100, ov}) == kErrInvalidParameter);
  }
  // Message box (Ex): the result is the active button.
  {
    const uint32_t ev = NewEvent(), ov = NewOverlapped(ev), result = e.Alloc(4), buttons = e.Alloc(8);
    Put32(buttons, e.WStr(u"Yes"));
    Put32(buttons + 4, e.WStr(u"No"));
    // (user, title, text, button_count, buttons, active_button, flags, unused, result*, overlapped)
    CHECK(Call(__imp__XamShowMessageBoxUIEx,
               {0, e.WStr(u"Title"), e.WStr(u"Text"), 2, buttons, 1, 0, 0, result, ov}) == kErrIoPending);
    CHECK(Get32(result) == 1);
    CHECK(Get32(ov) == 0);
    CHECK(Signaled(ev));
  }
  // Sign-in UI: returns 0.
  CHECK(Call(__imp__XamShowSigninUI, {1, 0}) == 0);
}

TEST_CASE("xam: the completion routine becomes an APC of the calling thread") {
  Env& e = E();
  g_ui.device_out = e.Alloc(4);
  g_ui.overlapped = NewOverlapped(0, kFnRoutine);
  g_ui.timeout = e.Alloc(8);
  Put64(g_ui.timeout, uint64_t(-int64_t(5000) * 10000));
  const int before = g_routine_count;
  auto t = GuestThread::Create(kFnUiApc, 0, 0, false);
  REQUIRE(t);
  const std::shared_ptr<KernelObject> one[1] = {t};
  const int64_t limit = -int64_t(10000) * 10000;
  REQUIRE(WaitAny(one, &limit, false) == kStatusWait0);
  CHECK(g_ui_status == kErrIoPending);
  CHECK(g_wait_status == kStatusUserApc);
  CHECK(g_routine_count == before + 1);
  CHECK(g_routine_args[0] == 0);               // result
  CHECK(g_routine_args[1] == 0);               // length (0 on success)
  CHECK(g_routine_args[2] == g_ui.overlapped);  // pointer to the overlapped
  CHECK(g_routine_thread == t.get());
  CHECK(Get32(g_ui.device_out) == 1);
}

// ---- notifications --------------------------------------------------------------

TEST_CASE("xam: two initial notifications, then none") {
  Env& e = E();
  xam::Reset();
  const uint32_t id = e.Alloc(4), param = e.Alloc(4);
  const uint32_t h = Call(__imp__XamNotifyCreateListener, {1 /* system area */, 10});
  REQUIRE(h != 0);
  CHECK(Signaled(h));
  CHECK(Call(__imp__XNotifyGetNext, {h, 0, id, param}) == 1);
  CHECK(Get32(id) == kXnSysSigninChanged);
  CHECK(Get32(param) == 1);
  CHECK(Call(__imp__XNotifyGetNext, {h, 0, id, param}) == 1);
  CHECK(Get32(id) == kXnSysStorageDevicesChanged);
  CHECK(Call(__imp__XNotifyGetNext, {h, 0, id, param}) == 0);
  CHECK(Get32(id) == 0);
  CHECK(Get32(param) == 0);
  CHECK(!Signaled(h));
  // param_ptr may be null (Xenia #1577).
  CHECK(Call(__imp__XNotifyGetNext, {h, 0, id, 0}) == 0);

  // A second listener in turn receives the two initial ones, and match_id
  // extracts a specific notification.
  const uint32_t h2 = Call(__imp__XamNotifyCreateListener, {0xFFFFFFFFull, 10});
  CHECK(Call(__imp__XNotifyGetNext, {h2, kXnSysStorageDevicesChanged, id, param}) == 1);
  CHECK(Get32(id) == kXnSysStorageDevicesChanged);
  CHECK(Call(__imp__XNotifyGetNext, {h2, kXnSysStorageDevicesChanged, id, param}) == 0);
  CHECK(Call(__imp__XNotifyGetNext, {h2, 0, id, param}) == 1);
  CHECK(Get32(id) == kXnSysSigninChanged);

  // An area other than the system one receives nothing at the start.
  const uint32_t h3 = Call(__imp__XamNotifyCreateListener, {2, 10});
  CHECK(Call(__imp__XNotifyGetNext, {h3, 0, id, param}) == 0);
  CHECK(!Signaled(h3));
  // XamShowSigninUI notifies the change to the listeners.
  Call(__imp__XamShowSigninUI, {0, 0});
  CHECK(Call(__imp__XNotifyGetNext, {h, kXnSysSigninChanged, id, param}) == 1);
  CHECK(Call(__imp__XNotifyGetNext, {h3, 0, id, param}) == 0);  // area 1 not in its mask

  // Unknown handle: 0, like Xenia. NtClose closes the listener.
  CHECK(Call(__imp__XNotifyGetNext, {0x7777, 0, id, param}) == 0);
  CHECK(Call(__imp__NtClose, {h}) == 0);
  CHECK(Call(__imp__NtClose, {h2}) == 0);
  CHECK(Call(__imp__NtClose, {h3}) == 0);
}

TEST_CASE("xam: a thread waiting on the listener wakes up with the notification") {
  Env& e = E();
  xam::Reset();
  const uint32_t id = e.Alloc(4), param = e.Alloc(4);
  const uint32_t h = Call(__imp__XamNotifyCreateListener, {1, 10});
  while (Call(__imp__XNotifyGetNext, {h, 0, id, param})) {}
  auto obj = HandleGet(h);
  REQUIRE(obj);
  std::atomic<bool> woke{false};
  std::thread waiter([&] {
    const std::shared_ptr<KernelObject> one[1] = {obj};
    const int64_t limit = -int64_t(5000) * 10000;
    woke = WaitAny(one, &limit, false) == kStatusWait0;
  });
  std::this_thread::sleep_for(50ms);
  xam::BroadcastNotification(kXnSysSigninChanged, 3);
  waiter.join();
  CHECK(woke);
  CHECK(Call(__imp__XNotifyGetNext, {h, 0, id, param}) == 1);
  CHECK(Get32(param) == 3);
  Call(__imp__NtClose, {h});
}

// ---- language and information --------------------------------------------------------

TEST_CASE("xam: language from code and XGetLanguage") {
  xam::Reset();
  CHECK(Call(__imp__XGetLanguage, {}) == 1);  // default: English
  struct Row { const char* code; uint32_t value; };
  for (Row r : {Row{"en", 1}, {"ja", 2}, {"de", 3}, {"fr", 4}, {"es", 5}, {"it", 6}, {"ko", 7}, {"zh", 8},
                {"pt", 9}, {"zh-cn", 10}, {"pl", 11}, {"ru", 12}, {"IT", 6}}) {
    const auto v = xam::LanguageFromCode(r.code);
    REQUIRE(v);
    CHECK(*v == r.value);
  }
  CHECK(!xam::LanguageFromCode("xx"));
  CHECK(!xam::LanguageFromCode(""));
  CHECK(!xam::LanguageFromCode("english"));
  xam::SetLanguage(*xam::LanguageFromCode("it"));
  CHECK(Call(__imp__XGetLanguage, {}) == 6);
  xam::Reset();
  CHECK(Call(__imp__XGetLanguage, {}) == 1);
}

TEST_CASE("xam: launch data absent and 1280x720 video mode") {
  Env& e = E();
  const uint32_t size = e.Alloc(4);
  Put32(size, 0x77);
  CHECK(Call(__imp__XamLoaderGetLaunchDataSize, {size}) == 0x490 /* ERROR_NOT_FOUND */);
  CHECK(Get32(size) == 0);
  const uint32_t mode = e.Alloc(48);
  Call(__imp__XGetVideoMode, {mode});
  CHECK(Get32(mode) == 1280);
  CHECK(Get32(mode + 4) == 720);
  CHECK(Get32(mode + 12) == 1);  // widescreen
  CHECK(Get32(mode + 16) == 1);  // hi-def
}

// ---- stubs: network, audio, XMA, voice ----------------------------------------------------

TEST_CASE("stub: the network answers not connected") {
  Env& e = E();
  CHECK(Call(__imp__NetDll_XNetStartup, {1, 0}) == 0);
  {
    const uint32_t rnd = e.Alloc(8);
    CHECK(Call(__imp__NetDll_XNetRandom, {1, rnd, 8}) == 0);
    CHECK(Get32(rnd) == 0xBBBBBBBB);
    CHECK(Call(__imp__NetDll_XNetRandom, {1, 0xFFFFFFF0, 0x100}) == 0x2726);  // beyond the guest space
  }
  const uint32_t wsa = e.Alloc(400);
  CHECK(Call(__imp__NetDll_WSAStartup, {1, 0x0202, wsa}) == 0);
  CHECK(uint16_t(*GuestPtr<be<uint16_t>>(wsa)) == 0x0202);
  const uint32_t xn = e.Alloc(36);
  std::memset(g_base + xn, 0xAA, 36);
  CHECK(Call(__imp__NetDll_XNetGetTitleXnAddr, {1, xn}) == 1);  // XNET_GET_XNADDR_NONE
  for (uint32_t i = 0; i < 36; ++i) CHECK(g_base[xn + i] == 0);  // no address
  CHECK(Call(__imp__NetDll_XNetGetEthernetLinkStatus, {1}) == 0);
  // Handle that is not a socket: WSAENOTSOCK (sockets come further below).
  CHECK(Call(__imp__NetDll_send, {1, 0x1234, e.CStr("x"), 1, 0}) == 0xFFFFFFFFu);
  CHECK(Call(__imp__NetDll_WSAGetLastError, {}) == 10038);  // WSAENOTSOCK

  CHECK(Call(__imp__NetDll_inet_addr, {e.CStr("192.168.1.20")}) == 0xC0A80114u);
  CHECK(Call(__imp__NetDll_inet_addr, {e.CStr("")}) == 0);
  CHECK(Call(__imp__NetDll_inet_addr, {e.CStr("not-an-ip")}) == 0xFFFFFFFFu);
  const uint32_t str = e.Alloc(32);
  Call(__imp__NetDll_XNetInAddrToString, {1, 0x0A000001, str, 32});
  CHECK(std::string(GuestPtr<char>(str)) == "10.0.0.1");

  // DNS: no result, the event is signaled and the structure is released.
  const uint32_t ev = NewEvent(), pdns = e.Alloc(4);
  CHECK(Call(__imp__NetDll_XNetDnsLookup, {1, e.CStr("example.com"), ev, pdns}) == 0);
  CHECK(Get32(pdns) != 0);
  CHECK(Get32(Get32(pdns)) != 0);  // iStatus != 0: error
  CHECK(Signaled(ev));
  CHECK(Call(__imp__NetDll_XNetDnsRelease, {1, Get32(pdns)}) == 0);
}

// ---- local sockets with no traffic ---------------------------------------------------

namespace {

constexpr uint32_t kSockErr = 0xFFFFFFFFu;
constexpr uint32_t kSolSocket = 0xFFFF, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoBroadcast = 0x20;
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;

uint32_t LastWsa() { return Call(__imp__NetDll_WSAGetLastError, {}); }
uint32_t NewSocket(uint32_t type = 2, uint32_t proto = 0xFE) {
  const uint32_t s = Call(__imp__NetDll_socket, {1, 2, type, proto});
  REQUIRE(s != kSockErr);
  return s;
}
// sockaddr_in: family u16, port u16 (big endian), addr u32, zero[8].
uint32_t SockAddr(uint16_t port, uint32_t addr = 0) {
  const uint32_t a = E().Alloc(16);
  *GuestPtr<be<uint16_t>>(a) = uint16_t(2);
  *GuestPtr<be<uint16_t>>(a + 2) = port;
  Put32(a + 4, addr);
  return a;
}
uint32_t Bind(uint32_t s, uint16_t port) { return Call(__imp__NetDll_bind, {1, s, SockAddr(port), 16}); }
uint16_t BoundPort(uint32_t s) {
  Env& e = E();
  const uint32_t name = e.Alloc(16), len = e.Alloc(4);
  Put32(len, 16);
  REQUIRE(Call(__imp__NetDll_getsockname, {1, s, name, len}) == 0);
  CHECK(Get32(len) == 16);
  CHECK(uint16_t(*GuestPtr<be<uint16_t>>(name)) == 2);
  CHECK(Get32(name + 4) == 0);  // address 0
  return *GuestPtr<be<uint16_t>>(name + 2);
}

}  // namespace

TEST_CASE("socket: socket() creates a handle for AF_INET and the supported types") {
  // Like the campaign: socket(AF_INET, SOCK_DGRAM, 0xFE /*VDP*/).
  const uint32_t s = Call(__imp__NetDll_socket, {1, 2, 2, 0xFE});
  CHECK(s != kSockErr);
  CHECK(s != 0);
  for (uint32_t type : {1u, 2u})
    for (uint32_t proto : {0u, 6u, 17u, 0xFEu}) {
      const uint32_t t = Call(__imp__NetDll_socket, {1, 2, type, proto});
      CHECK(t != kSockErr);
      CHECK(Call(__imp__NetDll_closesocket, {1, t}) == 0);
    }
  CHECK(Call(__imp__NetDll_socket, {1, 23, 2, 0}) == kSockErr);  // AF_INET6
  CHECK(LastWsa() == 10047);                                       // WSAEAFNOSUPPORT
  CHECK(Call(__imp__NetDll_socket, {1, 2, 3, 0}) == kSockErr);   // SOCK_RAW
  CHECK(LastWsa() == 10044);                                       // WSAESOCKTNOSUPPORT
  CHECK(Call(__imp__NetDll_socket, {1, 2, 2, 99}) == kSockErr);
  CHECK(LastWsa() == 10043);  // WSAEPROTONOSUPPORT
  CHECK(Call(__imp__NetDll_closesocket, {1, s}) == 0);
}

TEST_CASE("socket: setsockopt and getsockopt remember the values") {
  Env& e = E();
  const uint32_t s = NewSocket();
  const uint32_t val = e.Alloc(4), len = e.Alloc(4);
  Put32(val, 0x10000);
  CHECK(Call(__imp__NetDll_setsockopt, {1, s, kSolSocket, kSoSndBuf, val, 4}) == 0);
  Put32(val, 0x20000);
  CHECK(Call(__imp__NetDll_setsockopt, {1, s, kSolSocket, kSoRcvBuf, val, 4}) == 0);
  Put32(val, 0);
  Put32(len, 4);
  CHECK(Call(__imp__NetDll_getsockopt, {1, s, kSolSocket, kSoSndBuf, val, len}) == 0);
  CHECK(Get32(val) == 0x10000);
  CHECK(Get32(len) == 4);
  CHECK(Call(__imp__NetDll_getsockopt, {1, s, kSolSocket, kSoRcvBuf, val, len}) == 0);
  CHECK(Get32(val) == 0x20000);
  // Never set: buffer 65536, the rest 0.
  const uint32_t t = NewSocket();
  Put32(val, 0x77);
  Put32(len, 4);
  CHECK(Call(__imp__NetDll_getsockopt, {1, t, kSolSocket, kSoSndBuf, val, len}) == 0);
  CHECK(Get32(val) == 65536);
  CHECK(Get32(len) == 4);
  CHECK(Call(__imp__NetDll_getsockopt, {1, t, kSolSocket, kSoRcvBuf, val, len}) == 0);
  CHECK(Get32(val) == 65536);
  Put32(val, 0x77);
  CHECK(Call(__imp__NetDll_getsockopt, {1, t, kSolSocket, kSoBroadcast, val, len}) == 0);
  CHECK(Get32(val) == 0);
  Call(__imp__NetDll_closesocket, {1, s});
  Call(__imp__NetDll_closesocket, {1, t});
}

TEST_CASE("socket: bind, assigned port, contention and reuse after closesocket") {
  const uint32_t a = NewSocket(), b = NewSocket(), c = NewSocket();
  // Without bind getsockname has no name.
  {
    Env& e = E();
    const uint32_t name = e.Alloc(16), len = e.Alloc(4);
    Put32(len, 16);
    CHECK(Call(__imp__NetDll_getsockname, {1, a, name, len}) == kSockErr);
    CHECK(LastWsa() == 10022);  // WSAEINVAL
  }
  // Port 0: assigned by the counter, ephemeral.
  CHECK(Bind(a, 0) == 0);
  CHECK(BoundPort(a) >= 49152);
  // Explicit port, then a second socket on the same port (co-op host and client).
  CHECK(Bind(b, 3074) == 0);
  CHECK(BoundPort(b) == 3074);
  CHECK(Bind(c, 3074) == kSockErr);
  CHECK(LastWsa() == 10048);  // WSAEADDRINUSE
  // Freed with closesocket, the port is reused.
  CHECK(Call(__imp__NetDll_closesocket, {1, b}) == 0);
  CHECK(Bind(c, 3074) == 0);
  CHECK(BoundPort(c) == 3074);
  // Two binds to port 0 give different ports.
  const uint32_t d = NewSocket();
  CHECK(Bind(d, 0) == 0);
  CHECK(BoundPort(d) >= 49152);
  CHECK(BoundPort(d) != BoundPort(a));
  for (uint32_t s : {a, c, d}) Call(__imp__NetDll_closesocket, {1, s});
}

TEST_CASE("socket: ioctlsocket, no incoming data, sends discarded") {
  Env& e = E();
  const uint32_t s = NewSocket();
  REQUIRE(Bind(s, 0) == 0);
  const uint32_t arg = e.Alloc(4);
  Put32(arg, 1);
  CHECK(Call(__imp__NetDll_ioctlsocket, {1, s, kFionbio, arg}) == 0);
  Put32(arg, 0x55);
  CHECK(Call(__imp__NetDll_ioctlsocket, {1, s, kFionread, arg}) == 0);
  CHECK(Get32(arg) == 0);
  CHECK(Call(__imp__NetDll_ioctlsocket, {1, s, 0x12345678, arg}) == kSockErr);
  CHECK(LastWsa() == 10022);  // WSAEINVAL

  const uint32_t buf = e.Alloc(64), from = e.Alloc(16), fromlen = e.Alloc(4);
  Put32(fromlen, 16);
  // Blocking socket (t: no FIONBIO): recvfrom returns immediately, does not block.
  const uint32_t t = NewSocket();
  REQUIRE(Bind(t, 0) == 0);
  const auto start = std::chrono::steady_clock::now();
  CHECK(Call(__imp__NetDll_recvfrom, {1, t, buf, 64, 0, from, fromlen}) == kSockErr);
  CHECK(LastWsa() == 10035);  // WSAEWOULDBLOCK
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK(Call(__imp__NetDll_recvfrom, {1, s, buf, 64, 0, from, fromlen}) == kSockErr);
  CHECK(LastWsa() == 10035);
  CHECK(Call(__imp__NetDll_recv, {1, t, buf, 64, 0}) == kSockErr);
  CHECK(LastWsa() == 10035);
  // WSABUF: len u32, buf u32.
  const uint32_t wsabuf = e.Alloc(16), n = e.Alloc(4), flags = e.Alloc(4);
  Put32(wsabuf, 10);
  Put32(wsabuf + 4, buf);
  Put32(wsabuf + 8, 22);
  Put32(wsabuf + 12, buf);
  CHECK(Call(__imp__NetDll_WSARecv, {1, t, wsabuf, 2, n, flags, 0, 0}) == kSockErr);
  CHECK(LastWsa() == 10035);
  CHECK(Call(__imp__NetDll_WSARecvFrom, {1, t, wsabuf, 2, n, flags, from, fromlen, 0, 0}) == kSockErr);
  CHECK(LastWsa() == 10035);
  CHECK(Call(__imp__NetDll_accept, {1, t, from, fromlen}) == kSockErr);
  CHECK(LastWsa() == 10035);

  // Sends: succeed and the data is lost.
  const uint32_t to = SockAddr(3074, 0x0A000001);
  CHECK(Call(__imp__NetDll_sendto, {1, s, buf, 48, 0, to, 16}) == 48);
  CHECK(Call(__imp__NetDll_send, {1, s, buf, 5, 0}) == 5);
  Put32(n, 0);
  CHECK(Call(__imp__NetDll_WSASend, {1, s, wsabuf, 2, n, 0, 0, 0}) == 0);
  CHECK(Get32(n) == 32);
  Put32(n, 0);
  CHECK(Call(__imp__NetDll_WSASendTo, {1, s, wsabuf, 2, n, 0, to, 16, 0, 0}) == 0);
  CHECK(Get32(n) == 32);

  // connect, getpeername: no reachable peer.
  CHECK(Call(__imp__NetDll_connect, {1, s, to, 16}) == kSockErr);
  CHECK(LastWsa() == 10051);  // WSAENETUNREACH
  CHECK(Call(__imp__NetDll_getpeername, {1, s, from, fromlen}) == kSockErr);
  CHECK(LastWsa() == 10057);  // WSAENOTCONN
  // listen: STREAM only.
  CHECK(Call(__imp__NetDll_listen, {1, s, 5}) == kSockErr);
  CHECK(LastWsa() == 10045);  // WSAEOPNOTSUPP
  const uint32_t st = NewSocket(1, 6);
  CHECK(Call(__imp__NetDll_listen, {1, st, 5}) == 0);
  // shutdown, WSAEventSelect, WSACancelOverlappedIO: succeed.
  CHECK(Call(__imp__NetDll_shutdown, {1, s, 2}) == 0);
  CHECK(Call(__imp__NetDll_WSAEventSelect, {1, s, NewEvent(), 1}) == 0);
  CHECK(Call(__imp__NetDll_WSACancelOverlappedIO, {1, s}) == 0);

  // Closed: every call on that handle is WSAENOTSOCK.
  CHECK(Call(__imp__NetDll_closesocket, {1, s}) == 0);
  CHECK(Call(__imp__NetDll_sendto, {1, s, buf, 48, 0, to, 16}) == kSockErr);
  CHECK(LastWsa() == 10038);
  CHECK(Call(__imp__NetDll_closesocket, {1, s}) == kSockErr);
  CHECK(LastWsa() == 10038);
  for (uint32_t x : {t, st}) Call(__imp__NetDll_closesocket, {1, x});
}

TEST_CASE("socket: select zeroes read and exceptions, leaves write") {
  Env& e = E();
  const uint32_t s = NewSocket();
  // fd_set: fd_count u32, fd_array[64] u32.
  const uint32_t rd = e.Alloc(4 + 64 * 4), wr = e.Alloc(4 + 64 * 4), ex = e.Alloc(4 + 64 * 4);
  for (uint32_t set : {rd, wr, ex}) {
    Put32(set, 1);
    Put32(set + 4, s);
  }
  const uint32_t tv = e.Alloc(8);
  CHECK(Call(__imp__NetDll_select, {1, 0, rd, wr, ex, tv}) == 1);
  CHECK(Get32(rd) == 0);
  CHECK(Get32(ex) == 0);
  CHECK(Get32(wr) == 1);
  CHECK(Get32(wr + 4) == s);
  // Read only: no socket ready.
  Put32(rd, 1);
  CHECK(Call(__imp__NetDll_select, {1, 0, rd, 0, 0, tv}) == 0);
  CHECK(Get32(rd) == 0);
  Call(__imp__NetDll_closesocket, {1, s});
}

TEST_CASE("socket: a handle that is not a socket is rejected") {
  Env& e = E();
  const uint32_t buf = e.Alloc(8);
  // A WSA event is not a socket.
  const uint32_t ev = Call(__imp__NetDll_WSACreateEvent, {});
  REQUIRE(ev != 0);
  CHECK(Call(__imp__NetDll_send, {1, ev, buf, 1, 0}) == kSockErr);
  CHECK(LastWsa() == 10038);
  CHECK(Call(__imp__NetDll_bind, {1, ev, SockAddr(0), 16}) == kSockErr);
  CHECK(LastWsa() == 10038);
  CHECK(Call(__imp__NetDll_closesocket, {1, ev}) == kSockErr);
  CHECK(LastWsa() == 10038);
  CHECK(Call(__imp__NetDll_WSACloseEvent, {ev}) == 1);  // the event is still alive
  // Closed socket handle: it is no longer a socket.
  const uint32_t s = NewSocket();
  REQUIRE(Call(__imp__NetDll_closesocket, {1, s}) == 0);
  CHECK(Call(__imp__NetDll_recv, {1, s, buf, 8, 0}) == kSockErr);
  CHECK(LastWsa() == 10038);
}

TEST_CASE("stub: real WSA events, wait with timeout") {
  Env& e = E();
  const uint32_t h = Call(__imp__NetDll_WSACreateEvent, {});
  REQUIRE(h != 0);
  const uint32_t arr = e.Alloc(4);
  Put32(arr, h);
  // WSAWaitForMultipleEvents(count, events*, wait_all, timeout_ms, alertable): times out.
  CHECK(Call(__imp__NetDll_WSAWaitForMultipleEvents, {1, arr, 0, 20, 0}) == 0x102);
  CHECK(Call(__imp__NetDll_WSASetEvent, {h}) == 1);
  CHECK(Call(__imp__NetDll_WSAWaitForMultipleEvents, {1, arr, 0, 20, 0}) == 0);
  CHECK(Call(__imp__NetDll_WSAResetEvent, {h}) == 1);
  CHECK(Call(__imp__NetDll_WSAWaitForMultipleEvents, {1, arr, 0, 20, 0}) == 0x102);
  CHECK(Call(__imp__NetDll_WSACloseEvent, {h}) == 1);
  CHECK(Call(__imp__NetDll_WSACloseEvent, {h}) == 0);
  CHECK(Call(__imp__NetDll_WSAWaitForMultipleEvents, {65, arr, 0, 20, 0}) == 0xFFFFFFFFu);
}

// XAudio*: tests/apu/audio_system_test.cpp (runtime/apu).
TEST_CASE("stub: XMA gives valid contexts that play nothing") {
  Env& e = E();
  const uint32_t ctx_out = e.Alloc(4), ctx2_out = e.Alloc(4);
  CHECK(Call(__imp__XMACreateContext, {ctx_out}) == 0);
  CHECK(Call(__imp__XMACreateContext, {ctx2_out}) == 0);
  const uint32_t c1 = Get32(ctx_out), c2 = Get32(ctx2_out);
  CHECK(c1 != 0);
  CHECK(c2 != 0);
  CHECK(c1 != c2);
  CHECK(c1 >= kPhysicalHeapBegin);
  std::memset(g_base + c1, 0x5A, 64);  // the game writes into the context: the memory is its own
  CHECK(Call(__imp__XMAReleaseContext, {c1}) == 0);
  CHECK(Call(__imp__XMAReleaseContext, {c2}) == 0);
}

TEST_CASE("stub: voice without a device") {
  Env& e = E();
  const uint32_t voice = e.Alloc(4);
  Put32(voice, 0x1234);
  CHECK(Call(__imp__XamVoiceCreate, {0, 0xF, voice}) == kErrAccessDenied);
  CHECK(Get32(voice) == 0);
  CHECK(Call(__imp__XamVoiceHeadsetPresent, {0}) == 0);
  CHECK(Call(__imp__XamVoiceIsActiveProcess, {}) == 0);
}

TEST_CASE("xam: XamUserGetDeviceContext zeroes the context, pad only for user 0 / 0xFF") {
  Env& e = E();
  const uint32_t ctx = e.Alloc(4);
  for (uint32_t user : {0u, 0xFFu}) {
    Put32(ctx, 0x1234);
    CHECK(Call(__imp__XamUserGetDeviceContext, {user, 0, ctx}) == 0);
    CHECK(Get32(ctx) == 0);
  }
  Put32(ctx, 0x1234);
  CHECK(Call(__imp__XamUserGetDeviceContext, {1, 0, ctx}) == 0x48F);  // ERROR_DEVICE_NOT_CONNECTED
  CHECK(Get32(ctx) == 0);
}

TEST_CASE("xam: XMsg to XLiveBase (0xFC) offline, other apps") {
  Env& e = E();
  const uint32_t buf = e.Alloc(4);
  Put32(buf, 0);
  CHECK(Call(__imp__XMsgInProcessCall, {0xFC, 0x58004, buf, 4}) == 0);  // XLiveBaseGetLogonId
  CHECK(Get32(buf) == 1);
  CHECK(Call(__imp__XMsgInProcessCall, {0xFC, 0x58007, buf, 4}) == 0x80151802);
  CHECK(Call(__imp__XMsgInProcessCall, {0xFC, 0x12345, buf, 4}) == 0x80004005);
  CHECK(Call(__imp__XMsgInProcessCall, {0xFB, 0xB0001, buf, 4}) == 0x80004005);
  CHECK(Call(__imp__XMsgInProcessCall, {0x77, 1, buf, 4}) == 0x80070490);
  // async: result in the overlapped, ERROR_IO_PENDING; event signaled
  const uint32_t ev = NewEvent(), ov = NewOverlapped(ev);
  CHECK(Call(__imp__XMsgStartIORequest, {0xFC, 0x58006, ov, buf, 4}) == kErrIoPending);
  CHECK(Get32(ov) == 0);
  CHECK(HandleGet(ev)->IsSignaled());
  CHECK(Call(__imp__XMsgStartIORequest, {0x77, 1, 0, buf, 4}) == 0x80070057);
  CHECK(Call(__imp__XMsgCancelIORequest, {ov, 1}) == 0);
}

TEST_CASE("xam: aggregate content enumerator through its X_KENUMERATOR and XAM's message 0x2000E") {
  Env& e = E();
  const uint32_t handle = e.Alloc(4), priv = e.Alloc(4);
  CHECK(Call(__imp__XamContentAggregateCreateEnumerator, {0xE000000000000001ull, 1, 2, 0, 0}) == kEInvalidArg);
  REQUIRE(Call(__imp__XamContentAggregateCreateEnumerator, {0xE000000000000001ull, 1, 2, 0, handle}) == 0);
  CHECK(Call(__imp__XamGetPrivateEnumStructureFromHandle, {0x12345678, priv}) == 0xC0000008);
  REQUIRE(Call(__imp__XamGetPrivateEnumStructureFromHandle, {Get32(handle), priv}) == 0);
  const uint32_t k = Get32(priv);
  CHECK(Get32(k) == 0xFE);           // app_id
  CHECK(Get32(k + 4) == 0x2000E);    // open_message
  CHECK(Get32(k + 8) == 0x20010);    // close_message
  CHECK(Get32(k + 16) == 1);         // items_per_enumerate
  CHECK(Get32(k + 24) == 0x58454E00);     // aggregate extra data: magic 'XEN\0'
  CHECK(Get32(k + 28) == Get32(handle));  // and handle
  // The title's enumerate request (XContentAggregate in the XDK libraries).
  const uint32_t msg = e.Alloc(32), buf = e.Alloc(0x200), length = e.Alloc(4);
  Put32(msg, Get32(k + 12));
  Put32(msg + 8, k + 24);
  Put32(msg + 12, buf);
  Put32(msg + 16, 0x200);
  Put32(msg + 20, 1);
  Put32(msg + 24, length);
  Put32(length, 0xFFFF);
  CHECK(Call(__imp__XMsgInProcessCall, {Get32(k), Get32(k + 4), msg, 0}) == 0x80070012);  // no content
  CHECK(Get32(length) == 0);
  // The reference taken for the caller is dropped once.
  CHECK(ObjectDereference(k));
  CHECK_FALSE(ObjectDereference(k));
  CHECK(Call(__imp__NtClose, {Get32(handle)}) == 0);
}

TEST_CASE("xam: XamTaskSchedule runs the task, XMsgCompleteIORequest wakes XamGetOverlappedResult") {
  Env& e = E();
  const uint32_t ev = NewEvent(), ov = NewOverlapped(ev), length = e.Alloc(4), thread = e.Alloc(4);
  Put32(ov, kErrIoPending);
  CHECK(Call(__imp__XamGetOverlappedResult, {NewOverlapped(), length, 1}) == 0xDEAD);  // already complete
  const uint32_t pending = NewOverlapped();
  Put32(pending, kErrIoPending);
  CHECK(Call(__imp__XamGetOverlappedResult, {pending, length, 1}) == 0x3E4);  // no event: ERROR_IO_INCOMPLETE
  g_task_overlapped = ov;
  REQUIRE(Call(__imp__XamTaskSchedule, {kFnTask, 0x1234, 0, thread}) == 0);
  CHECK(Get32(thread) != 0);
  CHECK(Call(__imp__XamGetOverlappedResult, {ov, length, 1}) == 0x80070012);
  CHECK(Get32(length) == 7);
  CHECK(Get32(ov + 0x18) == 0x65B);
  CHECK(g_task_message == 0x1234);
  // XamTaskCloseHandle releases the last reference of the ended task: the
  // thread object (and its guest stack) is destroyed.
  std::weak_ptr<KernelObject> task = HandleGet(Get32(thread));
  REQUIRE(!task.expired());
  {
    const std::shared_ptr<KernelObject> one[1] = {task.lock()};
    REQUIRE(WaitAny(one, nullptr, false) == kStatusWait0);  // the task thread has ended
  }
  CHECK(Call(__imp__XamTaskCloseHandle, {Get32(thread)}) == 0);
  for (int i = 0; i < 200 && !task.expired(); ++i) std::this_thread::sleep_for(5ms);
  CHECK(task.expired());
  CHECK(Call(__imp__XamTaskCloseHandle, {Get32(thread)}) == 0xC0000008);
}

TEST_CASE("xam: XMsg to XGI (0xFB) like Xenia's xgi_app.cc") {
  Env& e = E();
  const uint32_t buf = e.Alloc(64);
  for (uint32_t off = 0; off < 64; off += 4) Put32(buf + off, 0);
  // success with no effects
  for (uint32_t msg : {0xB0006u, 0xB0007u, 0xB0008u, 0xB0010u, 0xB0011u, 0xB0012u, 0xB0014u, 0xB0015u, 0xB0071u})
    CHECK(Call(__imp__XMsgInProcessCall, {0xFB, msg, buf, 24}) == 0);
  // XGIUserGetContext: zeroes the value in the context and answers X_E_FAIL
  const uint32_t ctx = e.Alloc(8);
  Put32(ctx, 0x1234);
  Put32(ctx + 4, 0xDEAD);
  Put32(buf + 16, ctx);
  CHECK(Call(__imp__XMsgInProcessCall, {0xFB, 0xB0041, buf, 32}) == 0x80004005);
  CHECK(Get32(ctx) == 0x1234);
  CHECK(Get32(ctx + 4) == 0);
  // null context pointer: no write, always X_E_FAIL
  Put32(buf + 16, 0);
  CHECK(Call(__imp__XMsgInProcessCall, {0xFB, 0xB0041, buf, 32}) == 0x80004005);
  // async: result in the overlapped
  const uint32_t ev = NewEvent(), ov = NewOverlapped(ev);
  CHECK(Call(__imp__XMsgStartIORequest, {0xFB, 0xB0006, ov, buf, 24}) == kErrIoPending);
  CHECK(Get32(ov) == 0);
  // not handled
  CHECK(Call(__imp__XMsgInProcessCall, {0xFB, 0xB0099, buf, 4}) == 0x80004005);
}

TEST_CASE("xam: XMsg to XMP (0xFA) like Xenia's xmp_app.cc") {
  Env& e = E();
  const uint32_t buf = e.Alloc(48), a = e.Alloc(4), b = e.Alloc(4), c = e.Alloc(4);
  auto call = [&](uint32_t msg) { return Call(__imp__XMsgInProcessCall, {0xFA, msg, buf, 0}); };
  // XMPSetPlaybackBehavior / XMPGetPlaybackBehavior
  Put32(buf, 2); Put32(buf + 4, 3); Put32(buf + 8, 4); Put32(buf + 12, 5);
  CHECK(call(0x70008) == 0);
  Put32(buf, 2); Put32(buf + 4, a); Put32(buf + 8, b); Put32(buf + 12, c);
  CHECK(call(0x70029) == 0);
  CHECK(Get32(a) == 3);
  CHECK(Get32(b) == 4);
  CHECK(Get32(c) == 5);
  Put32(buf + 4, 0); Put32(buf + 8, 0); Put32(buf + 12, 0);  // null pointers allowed
  CHECK(call(0x70029) == 0);
  // XMPSetPlaybackController / XMPGetPlaybackController
  Put32(buf, 2); Put32(buf + 4, 0); Put32(buf + 8, 1);
  CHECK(call(0x7001A) == 0);
  Put32(a, 9); Put32(b, 9);
  Put32(buf, 2); Put32(buf + 4, a); Put32(buf + 8, b);
  CHECK(call(0x7001B) == 0);
  CHECK(Get32(a) == 0);
  CHECK(Get32(b) == 0);
  // XMPGetStatus: idle
  Put32(a, 9);
  Put32(buf, 2); Put32(buf + 4, a);
  CHECK(call(0x70009) == 0);
  CHECK(Get32(a) == 0);
  // volume: default 1.0f, then the one that was set
  Put32(buf, 2); Put32(buf + 4, a);
  CHECK(call(0x7000B) == 0);
  CHECK(Get32(a) == 0x3F800000);
  Put32(buf, 2); Put32(buf + 4, 0x3F000000);
  CHECK(call(0x7000C) == 0);
  Put32(buf, 2); Put32(buf + 4, a);
  CHECK(call(0x7000B) == 0);
  CHECK(Get32(a) == 0x3F000000);
  // storage size for the playlist: 4 + count * 128
  Put32(buf, 2); Put32(buf + 4, 10); Put32(buf + 8, a);
  CHECK(call(0x7002E) == 0);
  CHECK(Get32(a) == 4 + 10 * 128);
  // controls without a playlist: success; next/previous -> X_E_NOTFOUND
  Put32(buf, 2); Put32(buf + 4, 0);
  CHECK(call(0x70003) == 0);
  CHECK(call(0x70004) == 0);
  CHECK(call(0x70005) == 0);
  CHECK(call(0x70006) == 0x80070490);
  CHECK(call(0x70007) == 0x80070490);
  // info without an active playlist and playlist (objects not present): X_E_FAIL
  CHECK(call(0x7000E) == 0x80004005);
  CHECK(call(0x7000D) == 0x80004005);
  CHECK(call(0x7003D) == 0x80004005);
  CHECK(call(0x70099) == 0x80004005);
}

TEST_CASE("xam: every language of the settings file is accepted", "[xam]") {
  for (const std::string_view code : settings::kLanguageCodes) CHECK(kernel::xam::LanguageFromCode(code));
}
