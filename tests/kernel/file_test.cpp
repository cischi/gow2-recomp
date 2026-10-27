// File system exports (runtime/kernel/io/file_exports.cpp) on mounted
// temporary folders: open, read with offset, EOF, size
// and attribute queries, directory listing, async read with event and with APC,
// write denied on the read-only mount, write on the writable mount,
// open relative to a directory, symbolic links, package counting,
// read into physical pages watched by the GPU (runtime/physical_watch).
// Synthetic guest structures in a real GuestMemory arena: no game bytes.
#include "kernel/registry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/io/vfs.h"
#include "kernel/memory.h"
#include "kernel/objects.h"
#include "kernel/stats.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "physical_watch.h"
#include "../temp_dir.h"

PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__FscSetCacheElementCount);
PPC_EXTERN_FUNC(__imp__NtOpenFile);
PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtQueryInformationFile);
PPC_EXTERN_FUNC(__imp__NtSetInformationFile);
PPC_EXTERN_FUNC(__imp__NtQueryFullAttributesFile);
PPC_EXTERN_FUNC(__imp__NtQueryDirectoryFile);
PPC_EXTERN_FUNC(__imp__NtQueryVolumeInformationFile);
PPC_EXTERN_FUNC(__imp__NtFlushBuffersFile);
PPC_EXTERN_FUNC(__imp__NtDeviceIoControlFile);
PPC_EXTERN_FUNC(__imp__ObCreateSymbolicLink);
PPC_EXTERN_FUNC(__imp__ObDeleteSymbolicLink);
PPC_EXTERN_FUNC(__imp__IoDismountVolumeByName);
PPC_EXTERN_FUNC(__imp__NtClose);
PPC_EXTERN_FUNC(__imp__NtCreateEvent);
PPC_EXTERN_FUNC(__imp__NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(__imp__KeDelayExecutionThread);
PPC_EXTERN_FUNC(__imp__ObReferenceObjectByHandle);
PPC_EXTERN_FUNC(__imp__ObDereferenceObject);

namespace fs = std::filesystem;
using namespace kernel;
using namespace kernel::io;
using namespace std::chrono_literals;

namespace {

// Console constants (Xenia xboxkrnl_io.cc, vfs/entry.h, base/filesystem.h).
constexpr uint32_t kGenericRead = 0x80000000;
constexpr uint32_t kGenericWrite = 0x40000000;
constexpr uint32_t kSynchronize = 0x00100000;
constexpr uint32_t kFileDirectoryFile = 0x01;
constexpr uint32_t kFileSynchronousIoNonalert = 0x20;
constexpr uint32_t kFileNonDirectoryFile = 0x40;
constexpr uint32_t kSupersede = 0, kOpen = 1, kCreate = 2, kOpenIf = 3, kOverwriteIf = 5;
constexpr uint32_t kActionOpened = 1, kActionCreated = 2;
constexpr uint32_t kFileBasicInformation = 4, kFileStandardInformation = 5, kFilePositionInformation = 14,
                   kFileEndOfFileInformation = 20, kFileNetworkOpenInformation = 34;
constexpr uint32_t kFileFsSizeInformation = 3;
constexpr uint32_t kAttrDirectory = 0x10;
constexpr uint32_t kObDosDevices = 0xFFFFFFFD;

constexpr char kContent[] = "0123456789ABCDEF";  // 16 bytes

uint32_t Get32(uint32_t a) { return *GuestPtr<be<uint32_t>>(a); }
uint64_t Get64(uint32_t a) { return *GuestPtr<be<uint64_t>>(a); }
void Put32(uint32_t a, uint32_t v) { *GuestPtr<be<uint32_t>>(a) = v; }
void Put64(uint32_t a, uint64_t v) { *GuestPtr<be<uint64_t>>(a) = v; }
std::string GetStr(uint32_t a, size_t n) { return std::string(GuestPtr<char>(a), n); }

// ---- fake guest functions for the APC -----------------------------------------

constexpr uint32_t kFnIoApc = 0x82000100;
constexpr uint32_t kFnAsyncRead = 0x82000110;

std::atomic<uint32_t> g_apc_args[3];
std::atomic<int> g_apc_count{0};
std::atomic<GuestThread*> g_apc_thread{nullptr};
PPC_FUNC(FnIoApc) {
  g_apc_args[0] = ctx.r3.u32;
  g_apc_args[1] = ctx.r4.u32;
  g_apc_args[2] = ctx.r5.u32;
  g_apc_thread = GuestThread::Current();
  ++g_apc_count;
}

struct AsyncArgs {
  uint32_t file, iosb, buffer, offset, timeout, routine;
};
AsyncArgs g_async;
std::atomic<uint32_t> g_read_status{0}, g_wait_status{0};
std::atomic<int> g_apc_before_wait{-1};

uint32_t CallRegs(PPCFunc* fn, std::initializer_list<uint64_t> args);

// NtReadFile with a completion routine, then an alertable wait.
PPC_FUNC(FnAsyncRead) {
  const AsyncArgs a = g_async;
  g_read_status = CallRegs(__imp__NtReadFile, {a.file, 0, a.routine, 0x1234, a.iosb, a.buffer, 4, a.offset});
  g_apc_before_wait = g_apc_count.load();  // the APC does not run inside NtReadFile
  g_wait_status = CallRegs(__imp__KeDelayExecutionThread, {1, 1, a.timeout});
  ctx.r3.u64 = 0;
}

PPCFunc* ResolveFn(uint32_t guest) {
  switch (guest) {
    case kFnIoApc: return &FnIoApc;
    case kFnAsyncRead: return &FnAsyncRead;
    default: return nullptr;
  }
}

// ---- environment --------------------------------------------------------------

void Touch(const fs::path& p, std::string_view content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

struct Env {
  GuestMemory mem;
  gow2_test::TempDir tmp{"gow2_file_test"};  // unique per process (ctest -j)
  fs::path base, game, cache;
  uint32_t stack = 0;  // area for the arguments from the ninth on (r1)
  Env() {
    REQUIRE(mem.Init());
    g_base = mem.base;
    REQUIRE(InitMemory(mem));
    REQUIRE(InitThreads(mem, &ResolveFn, 0x40000));
    base = tmp.path;
    game = base / "game";
    cache = base / "user_data" / "cache";
    Touch(game / "Readme.TXT", kContent);
    Touch(game / "GearGame" / "CookedXenon" / "Startup.xxx", "pkg");
    Touch(game / "GearGame" / "Movies" / "Intro.upk", "upk");
    Touch(game / "Dir" / "a.bin", "aa");
    Touch(game / "Dir" / "B.txt", "bbb");
    Touch(game / "Dir" / "Sub" / "c.txt", "c");
    Touch(base / "outside" / "secret.txt", "secret");
    fs::create_directories(cache);
    stack = Alloc(0x400);
  }
  void Mount() {
    SetMounts({{"game:", game, false}, {"\\Device\\Cdrom0", game, false}, {"cache:", cache, true}});
  }
  uint32_t Alloc(uint32_t size) {
    const uint32_t a = mem.Alloc(size, 0x10);
    REQUIRE(a != 0);
    std::memset(mem.base + a, 0, size);
    return a;
  }
  // Guest ANSI_STRING (Length, MaximumLength, Buffer).
  uint32_t Ansi(std::string_view s) {
    const uint32_t buf = Alloc(uint32_t(s.size() + 1));
    std::memcpy(mem.base + buf, s.data(), s.size());
    const uint32_t str = Alloc(8);
    *GuestPtr<be<uint16_t>>(str) = uint16_t(s.size());
    *GuestPtr<be<uint16_t>>(str + 2) = uint16_t(s.size() + 1);
    Put32(str + 4, buf);
    return str;
  }
  // Guest OBJECT_ATTRIBUTES (RootDirectory, ObjectName, Attributes).
  uint32_t Attrs(std::string_view path, uint32_t root = 0) {
    const uint32_t oa = Alloc(12);
    Put32(oa, root);
    Put32(oa + 4, Ansi(path));
    Put32(oa + 8, 0x40);  // OBJ_CASE_INSENSITIVE
    return oa;
  }
};

Env& E() {
  static Env env;
  return env;
}

// Calls an export like the generated code: r3..r10, then the guest stack
// (r1 + 0x54 + (i-8)*8) for the further arguments.
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

struct Opened {
  uint32_t status = 0, handle = 0, iosb = 0;
};

Opened Create(std::string_view path, uint32_t access, uint32_t disposition, uint32_t options, uint32_t root = 0) {
  Env& e = E();
  Opened o;
  const uint32_t out = e.Alloc(4);
  o.iosb = e.Alloc(8);
  Put32(o.iosb, 0xDEAD);
  o.status = Call(__imp__NtCreateFile, {out, access, e.Attrs(path, root), o.iosb, 0, 0, 7, disposition, options});
  o.handle = Get32(out);
  return o;
}

Opened Open(std::string_view path, uint32_t options = kFileSynchronousIoNonalert | kFileNonDirectoryFile,
            uint32_t access = kGenericRead | kSynchronize) {
  Env& e = E();
  Opened o;
  const uint32_t out = e.Alloc(4);
  o.iosb = e.Alloc(8);
  o.status = Call(__imp__NtOpenFile, {out, access, e.Attrs(path), o.iosb, options});
  o.handle = Get32(out);
  return o;
}

struct Io {
  uint32_t status, iosb_status, info, buffer;
};

Io Read(uint32_t handle, uint32_t length, const int64_t* offset, uint32_t event = 0) {
  Env& e = E();
  const uint32_t iosb = e.Alloc(8);
  Put32(iosb, 0xDEAD);
  Put32(iosb + 4, 0xDEAD);
  const uint32_t buf = e.Alloc(length + 16);
  uint32_t off = 0;
  if (offset) {
    off = e.Alloc(8);
    Put64(off, uint64_t(*offset));
  }
  const uint32_t st = Call(__imp__NtReadFile, {handle, event, 0, 0, iosb, buf, length, off});
  return {st, Get32(iosb), Get32(iosb + 4), buf};
}

Io Write(uint32_t handle, std::string_view data, const int64_t* offset) {
  Env& e = E();
  const uint32_t iosb = e.Alloc(8);
  const uint32_t buf = e.Alloc(uint32_t(data.size() + 1));
  std::memcpy(e.mem.base + buf, data.data(), data.size());
  uint32_t off = 0;
  if (offset) {
    off = e.Alloc(8);
    Put64(off, uint64_t(*offset));
  }
  const uint32_t st = Call(__imp__NtWriteFile, {handle, 0, 0, 0, iosb, buf, uint32_t(data.size()), off});
  return {st, Get32(iosb), Get32(iosb + 4), buf};
}

uint64_t SummaryValue(const std::string& key) {
  const std::string s = stats::Summary();
  const std::regex re("\\[summary\\] " + key + "=(\\d+)\n");
  std::smatch m;
  return std::regex_search(s, m, re) ? std::stoull(m[1].str()) : ~0ull;
}

std::string ReadHost(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("file: open, read with offset and from the position, EOF, close") {
  Env& e = E();
  e.Mount();
  auto o = Open("game:\\README.txt");
  REQUIRE(o.status == kStatusSuccess);
  CHECK(o.handle != 0);
  CHECK(Get32(o.iosb) == kStatusSuccess);
  CHECK(Get32(o.iosb + 4) == kActionOpened);

  const int64_t ten = 10;
  auto r = Read(o.handle, 4, &ten);
  CHECK(r.status == kStatusSuccess);
  CHECK(r.iosb_status == kStatusSuccess);
  CHECK(r.info == 4);
  CHECK(GetStr(r.buffer, 4) == "ABCD");
  // without offset: from the current position (14), partial read
  r = Read(o.handle, 8, nullptr);
  CHECK(r.status == kStatusSuccess);
  CHECK(r.info == 2);
  CHECK(GetStr(r.buffer, 2) == "EF");
  // FILE_USE_FILE_POINTER_POSITION (-2) = current position: we are at end of file
  const int64_t use_pos = -2;
  r = Read(o.handle, 4, &use_pos);
  CHECK(r.status == kStatusEndOfFile);
  CHECK(r.iosb_status == kStatusEndOfFile);
  CHECK(r.info == 0);
  // beyond the end
  const int64_t beyond = 1000;
  r = Read(o.handle, 4, &beyond);
  CHECK(r.status == kStatusEndOfFile);
  // length 0: success
  r = Read(o.handle, 0, &ten);
  CHECK(r.status == kStatusSuccess);
  CHECK(r.info == 0);

  CHECK(Call(__imp__NtClose, {o.handle}) == kStatusSuccess);
  r = Read(o.handle, 4, &ten);
  CHECK(r.status == kStatusInvalidHandle);
}

TEST_CASE("file: NtCreateFile, dispositions, directories and invalid paths") {
  Env& e = E();
  e.Mount();
  const uint32_t sync = kFileSynchronousIoNonalert;
  auto missing = Create("game:\\nope.bin", kGenericRead, kOpen, sync);
  CHECK(missing.status == kStatusObjectNameNotFound);
  CHECK(Get32(missing.iosb) == kStatusObjectNameNotFound);
  CHECK(Create("game:\\NoDir\\nope.bin", kGenericRead, kOpen, sync).status == kStatusObjectPathNotFound);
  CHECK(Create("game:\\..\\outside\\secret.txt", kGenericRead, kOpen, sync).status == kStatusObjectNameInvalid);
  CHECK(Create("game:\\C:\\Windows\\win.ini", kGenericRead, kOpen, sync).status == kStatusObjectNameInvalid);
  CHECK(Create("game:\\Readme.TXT::$DATA", kGenericRead, kOpen, sync).status == kStatusObjectNameInvalid);
  CHECK(Create("game:\\CON", kGenericRead, kOpen, sync).status == kStatusObjectNameInvalid);
  CHECK(Create("nodev:\\x", kGenericRead, kOpen, sync).status == kStatusObjectPathNotFound);
  // wrong type
  CHECK(Create("game:\\Dir", kGenericRead, kOpen, kFileNonDirectoryFile).status == kStatusFileIsADirectory);
  CHECK(Create("game:\\Readme.TXT", kGenericRead, kOpen, kFileDirectoryFile).status == kStatusNotADirectory);
  // directory opened as a directory
  auto dir = Create("game:\\dir", kGenericRead, kOpen, kFileDirectoryFile | sync);
  CHECK(dir.status == kStatusSuccess);
  // reading a directory makes no sense
  const int64_t zero = 0;
  CHECK(Read(dir.handle, 4, &zero).status == kStatusInvalidParameter);
  CHECK(Call(__imp__NtClose, {dir.handle}) == kStatusSuccess);
  // null OBJECT_ATTRIBUTES
  const uint32_t out = e.Alloc(4);
  CHECK(Call(__imp__NtCreateFile, {out, kGenericRead, 0, e.Alloc(8), 0, 0, 0, kOpen, 0}) == kStatusInvalidParameter);
  // RootDirectory = ObDosDevices (0xFFFFFFFD): as absolute
  const uint32_t iosb = e.Alloc(8);
  CHECK(Call(__imp__NtCreateFile,
             {out, kGenericRead, e.Attrs("game:\\Readme.TXT", kObDosDevices), iosb, 0, 0, 0, kOpen, sync}) ==
        kStatusSuccess);
  Call(__imp__NtClose, {Get32(out)});
}

TEST_CASE("file: open relative to a directory (RootDirectory)") {
  Env& e = E();
  e.Mount();
  auto dir = Create("game:\\Dir", kGenericRead, kOpen, kFileDirectoryFile);
  REQUIRE(dir.status == kStatusSuccess);
  auto rel = Create("Sub\\..\\B.TXT", kGenericRead, kOpen, kFileSynchronousIoNonalert, dir.handle);
  CHECK(rel.status == kStatusSuccess);
  const int64_t zero = 0;
  auto r = Read(rel.handle, 8, &zero);
  CHECK(r.info == 3);
  CHECK(GetStr(r.buffer, 3) == "bbb");
  Call(__imp__NtClose, {rel.handle});
  // the root cannot be escaped even going through the directory
  CHECK(Create("..\\..\\outside\\secret.txt", kGenericRead, kOpen, 0, dir.handle).status == kStatusObjectNameInvalid);
  auto up = Create("..\\Readme.TXT", kGenericRead, kOpen, 0, dir.handle);
  CHECK(up.status == kStatusSuccess);
  Call(__imp__NtClose, {up.handle});
  // unknown directory handle
  CHECK(Create("B.txt", kGenericRead, kOpen, 0, 0x7FF0).status == kStatusInvalidHandle);
  Call(__imp__NtClose, {dir.handle});
}

TEST_CASE("file: shorter replaced file: original's size, zero tail (--skip-movie)") {
  Env& e = E();
  e.Mount();
  SetFileReplacements({{"B.txt", "a.bin"}});  // "bbb" (3 bytes) -> "aa" (2 bytes)
  auto o = Open("game:/Dir/b.txt");
  REQUIRE(o.status == kStatusSuccess);
  const int64_t zero = 0;
  auto r = Read(o.handle, 8, &zero);
  CHECK(r.status == kStatusSuccess);
  CHECK(r.info == 3);
  CHECK(GetStr(r.buffer, 3) == std::string("aa\0", 3));
  const int64_t three = 3;
  r = Read(o.handle, 4, &three);
  CHECK(r.status == kStatusEndOfFile);
  const uint32_t iosb = e.Alloc(8);
  const uint32_t info = e.Alloc(64);
  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 24, kFileStandardInformation}) == kStatusSuccess);
  CHECK(Get64(info + 8) == 3);  // EndOfFile
  CHECK(Call(__imp__NtClose, {o.handle}) == kStatusSuccess);
  SetFileReplacements({});
}

TEST_CASE("file: size, position and attribute queries") {
  Env& e = E();
  e.Mount();
  auto o = Open("game:\\Readme.TXT");
  REQUIRE(o.status == kStatusSuccess);
  const uint32_t iosb = e.Alloc(8);
  const uint32_t info = e.Alloc(64);

  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 56, kFileNetworkOpenInformation}) ==
        kStatusSuccess);
  CHECK(Get32(iosb + 4) == 56);
  CHECK(Get64(info + 0x28) == 16);  // EndOfFile
  CHECK(Get64(info + 0x20) >= 16);  // AllocationSize
  CHECK(Get64(info + 0x18) > 0);    // LastWriteTime (FILETIME)
  CHECK((Get32(info + 0x30) & kAttrDirectory) == 0);

  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 24, kFileStandardInformation}) == kStatusSuccess);
  CHECK(Get64(info + 8) == 16);       // EndOfFile
  CHECK(e.mem.base[info + 21] == 0);  // Directory

  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 40, kFileBasicInformation}) == kStatusSuccess);
  CHECK(Get64(info + 0x10) > 0);  // LastWriteTime

  // position: set and re-read, then a read without offset from there
  Put64(info, 12);
  CHECK(Call(__imp__NtSetInformationFile, {o.handle, iosb, info, 8, kFilePositionInformation}) == kStatusSuccess);
  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 8, kFilePositionInformation}) == kStatusSuccess);
  CHECK(Get64(info) == 12);
  auto r = Read(o.handle, 4, nullptr);
  CHECK(GetStr(r.buffer, 4) == "CDEF");

  // buffer too small, unknown class, unknown handle
  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 8, kFileNetworkOpenInformation}) ==
        kStatusInfoLengthMismatch);
  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, info, 64, 999}) == kStatusInvalidInfoClass);
  CHECK(Call(__imp__NtQueryInformationFile, {0x7FF0, iosb, info, 56, kFileNetworkOpenInformation}) ==
        kStatusInvalidHandle);

  // volume
  CHECK(Call(__imp__NtQueryVolumeInformationFile, {o.handle, iosb, info, 24, kFileFsSizeInformation}) ==
        kStatusSuccess);
  CHECK(Get32(info + 20) == 0x200);  // BytesPerSector
  CHECK(Get64(info) > 0);

  CHECK(Call(__imp__NtFlushBuffersFile, {o.handle, iosb}) == kStatusSuccess);
  CHECK(Call(__imp__NtDeviceIoControlFile, {o.handle, 0, 0, 0, iosb, 0x70000, 0, 0, 0, 0}) == kStatusNotImplemented);

  // the file is a kernel object: ObReferenceObjectByHandle works
  const uint32_t obj = e.Alloc(4);
  CHECK(Call(__imp__ObReferenceObjectByHandle, {o.handle, 0, obj}) == kStatusSuccess);
  CHECK(Get32(obj) != 0);
  Call(__imp__ObDereferenceObject, {Get32(obj)});
  Call(__imp__NtClose, {o.handle});

  // NtQueryFullAttributesFile: file, directory, missing, hostile
  CHECK(Call(__imp__NtQueryFullAttributesFile, {e.Attrs("GAME:\\readme.txt"), info}) == kStatusSuccess);
  CHECK(Get64(info + 0x28) == 16);
  CHECK((Get32(info + 0x30) & kAttrDirectory) == 0);
  CHECK(Call(__imp__NtQueryFullAttributesFile, {e.Attrs("game:\\GearGame"), info}) == kStatusSuccess);
  CHECK((Get32(info + 0x30) & kAttrDirectory) != 0);
  CHECK(Call(__imp__NtQueryFullAttributesFile, {e.Attrs("game:\\missing.bin"), info}) == kStatusObjectNameNotFound);
  CHECK(Call(__imp__NtQueryFullAttributesFile, {e.Attrs("game:\\..\\outside\\secret.txt"), info}) ==
        kStatusObjectNameInvalid);
}

TEST_CASE("file: directory listing with pattern") {
  Env& e = E();
  e.Mount();
  const uint32_t iosb = e.Alloc(8);
  const uint32_t info = e.Alloc(0x200);

  // Each listing on a new handle: the pattern stays the one from the first
  // call with a name (like NT and Xenia).
  auto list = [&](std::string_view pattern) {
    std::multiset<std::string> names;
    auto dir = Create("game:\\Dir", kGenericRead, kOpen, kFileDirectoryFile | kFileSynchronousIoNonalert);
    REQUIRE(dir.status == kStatusSuccess);
    uint32_t name = pattern.empty() ? 0 : e.Ansi(pattern);
    uint32_t restart = 1;
    for (int i = 0; i < 20; ++i) {
      const uint32_t st = Call(__imp__NtQueryDirectoryFile, {dir.handle, 0, 0, 0, iosb, info, 0x200, name, restart});
      if (st != kStatusSuccess) {
        CHECK((st == kStatusNoMoreFiles || st == kStatusNoSuchFile));
        break;
      }
      CHECK(Get32(info) == 0);  // NextEntryOffset: one entry per call
      const uint32_t len = Get32(info + 0x3C);
      std::string n = GetStr(info + 0x40, len);
      if (n == "Sub") CHECK((Get32(info + 0x38) & kAttrDirectory) != 0);
      if (n == "B.txt") CHECK(Get64(info + 0x28) == 3);
      names.insert(n);
      name = 0;  // the following calls continue the search
      restart = 0;
    }
    Call(__imp__NtClose, {dir.handle});
    return names;
  };
  CHECK(list("*") == std::multiset<std::string>{"B.txt", "Sub", "a.bin"});
  CHECK(list("*.*") == std::multiset<std::string>{"B.txt", "Sub", "a.bin"});
  CHECK(list("*.TXT") == std::multiset<std::string>{"B.txt"});
  CHECK(list("nothing*").empty());
  // without a name: everything
  CHECK(list("") == std::multiset<std::string>{"B.txt", "Sub", "a.bin"});

  auto dir = Create("game:\\Dir", kGenericRead, kOpen, kFileDirectoryFile | kFileSynchronousIoNonalert);
  REQUIRE(dir.status == kStatusSuccess);
  // the first call's pattern stays; restart starts over with that one
  CHECK(Call(__imp__NtQueryDirectoryFile, {dir.handle, 0, 0, 0, iosb, info, 0x200, e.Ansi("a*"), 0}) ==
        kStatusSuccess);
  CHECK(GetStr(info + 0x40, Get32(info + 0x3C)) == "a.bin");
  CHECK(Call(__imp__NtQueryDirectoryFile, {dir.handle, 0, 0, 0, iosb, info, 0x200, 0, 0}) == kStatusNoMoreFiles);
  CHECK(Call(__imp__NtQueryDirectoryFile, {dir.handle, 0, 0, 0, iosb, info, 0x200, 0, 1}) == kStatusSuccess);
  CHECK(GetStr(info + 0x40, Get32(info + 0x3C)) == "a.bin");
  CHECK(Get32(iosb) == kStatusSuccess);
  CHECK(Get32(iosb + 4) == 0x40 + 5);
  // buffer too small, pattern with separators, a file's handle
  CHECK(Call(__imp__NtQueryDirectoryFile, {dir.handle, 0, 0, 0, iosb, info, 16, 0, 1}) == kStatusInfoLengthMismatch);
  CHECK(Call(__imp__NtQueryDirectoryFile, {dir.handle, 0, 0, 0, iosb, info, 0x200, e.Ansi("..\\*"), 1}) ==
        kStatusInvalidParameter);
  Call(__imp__NtClose, {dir.handle});
  auto file = Open("game:\\Readme.TXT");
  CHECK(Call(__imp__NtQueryDirectoryFile, {file.handle, 0, 0, 0, iosb, info, 0x200, 0, 1}) == kStatusInvalidParameter);
  Call(__imp__NtClose, {file.handle});
}

TEST_CASE("file: async read with event") {
  Env& e = E();
  e.Mount();
  auto o = Open("game:\\Readme.TXT", 0);  // without FILE_SYNCHRONOUS_IO_*: async
  REQUIRE(o.status == kStatusSuccess);
  const uint32_t ev_out = e.Alloc(4);
  REQUIRE(Call(__imp__NtCreateEvent, {ev_out, 0, 0, 0}) == kStatusSuccess);  // manual, not signaled
  const uint32_t ev = Get32(ev_out);
  const uint32_t tmo = e.Alloc(8);
  Put64(tmo, 0);
  CHECK(Call(__imp__NtWaitForSingleObjectEx, {ev, 0, 0, tmo}) == kStatusTimeout);

  const int64_t two = 2;
  auto r = Read(o.handle, 4, &two, ev);
  CHECK(r.status == kStatusPending);  // like Xenia: completed immediately, reported as pending
  CHECK(r.iosb_status == kStatusSuccess);
  CHECK(r.info == 4);
  CHECK(GetStr(r.buffer, 4) == "2345");
  CHECK(Call(__imp__NtWaitForSingleObjectEx, {ev, 0, 0, tmo}) == kStatusSuccess);
  // the file too is waitable: signaled once the I/O has finished
  CHECK(Call(__imp__NtWaitForSingleObjectEx, {o.handle, 0, 0, tmo}) == kStatusSuccess);

  // async EOF: pending, with the real status in the IO_STATUS_BLOCK
  const int64_t beyond = 100;
  r = Read(o.handle, 4, &beyond, ev);
  CHECK(r.status == kStatusPending);
  CHECK(r.iosb_status == kStatusEndOfFile);
  CHECK(r.info == 0);

  // invalid event: no read
  r = Read(o.handle, 4, &two, 0x7FF0);
  CHECK(r.status == kStatusInvalidHandle);
  Call(__imp__NtClose, {ev});
  Call(__imp__NtClose, {o.handle});
}

TEST_CASE("file: async read with completion routine (APC on the calling thread)") {
  Env& e = E();
  e.Mount();
  auto o = Open("game:\\Readme.TXT", 0);
  REQUIRE(o.status == kStatusSuccess);
  for (uint32_t routine : {kFnIoApc, kFnIoApc | 1}) {  // the low bit is ignored (Xenia)
    INFO(routine);
    g_async.file = o.handle;
    g_async.iosb = e.Alloc(8);
    g_async.buffer = e.Alloc(16);
    g_async.offset = e.Alloc(8);
    Put64(g_async.offset, 4);
    g_async.timeout = e.Alloc(8);
    Put64(g_async.timeout, uint64_t(-int64_t(5000) * 10000));  // 5 s relative
    g_async.routine = routine;
    const int before = g_apc_count;
    g_apc_thread = nullptr;
    auto t = GuestThread::Create(kFnAsyncRead, 0, 0, false);
    REQUIRE(t);
    const std::shared_ptr<KernelObject> one[1] = {t};
    const int64_t limit = -int64_t(10000) * 10000;
    REQUIRE(WaitAny(one, &limit, false) == kStatusWait0);
    CHECK(g_read_status == kStatusPending);
    CHECK(g_apc_before_wait == before);
    CHECK(g_wait_status == kStatusUserApc);
    CHECK(g_apc_count == before + 1);
    CHECK(g_apc_args[0] == 0x1234);        // ApcContext
    CHECK(g_apc_args[1] == g_async.iosb);  // IoStatusBlock
    CHECK(g_apc_args[2] == 0);             // Reserved
    CHECK(g_apc_thread == t.get());
    CHECK(Get32(g_async.iosb) == kStatusSuccess);
    CHECK(Get32(g_async.iosb + 4) == 4);
    CHECK(GetStr(g_async.buffer, 4) == "4567");
  }
  Call(__imp__NtClose, {o.handle});
}

TEST_CASE("file: write denied on the read-only mount, allowed on cache:") {
  Env& e = E();
  e.Mount();
  // opened for writing on read-only: the open succeeds (Xenia downgrades it to
  // read), the write does not
  auto ro = Open("game:\\Readme.TXT", kFileSynchronousIoNonalert, kGenericRead | kGenericWrite);
  REQUIRE(ro.status == kStatusSuccess);
  const int64_t zero = 0;
  auto w = Write(ro.handle, "XX", &zero);
  CHECK(w.status == kStatusAccessDenied);
  CHECK(w.iosb_status == kStatusAccessDenied);
  CHECK(ReadHost(e.game / "Readme.TXT") == kContent);
  const uint32_t iosb = e.Alloc(8);
  const uint32_t info = e.Alloc(8);
  Put64(info, 2);
  CHECK(Call(__imp__NtSetInformationFile, {ro.handle, iosb, info, 8, kFileEndOfFileInformation}) ==
        kStatusAccessDenied);
  Call(__imp__NtClose, {ro.handle});
  // create or overwrite on the read-only mount
  CHECK(Create("game:\\new.bin", kGenericWrite, kCreate, 0).status == kStatusAccessDenied);
  CHECK(Create("game:\\new.bin", kGenericWrite, kOpenIf, 0).status == kStatusAccessDenied);
  CHECK(Create("game:\\Readme.TXT", kGenericWrite, kOverwriteIf, 0).status == kStatusAccessDenied);
  CHECK(Create("game:\\Readme.TXT", kGenericWrite, kSupersede, 0).status == kStatusAccessDenied);
  CHECK(Create("game:\\NewDir", kGenericRead, kCreate, kFileDirectoryFile).status == kStatusAccessDenied);
  CHECK_FALSE(fs::exists(e.game / "new.bin"));
  CHECK_FALSE(fs::exists(e.game / "NewDir"));
  CHECK(ReadHost(e.game / "Readme.TXT") == kContent);

  // cache: creation, write, re-read, collision, overwrite
  auto c = Create("cache:\\Save.DAT", kGenericRead | kGenericWrite, kCreate, kFileSynchronousIoNonalert);
  REQUIRE(c.status == kStatusSuccess);
  CHECK(Get32(c.iosb + 4) == kActionCreated);
  w = Write(c.handle, "hello", &zero);
  CHECK(w.status == kStatusSuccess);
  CHECK(w.info == 5);
  w = Write(c.handle, "!!", nullptr);  // from the current position
  CHECK(w.status == kStatusSuccess);
  auto r = Read(c.handle, 16, &zero);
  CHECK(r.info == 7);
  CHECK(GetStr(r.buffer, 7) == "hello!!");
  Put64(info, 4);
  CHECK(Call(__imp__NtSetInformationFile, {c.handle, iosb, info, 8, kFileEndOfFileInformation}) == kStatusSuccess);
  Call(__imp__NtClose, {c.handle});
  CHECK(ReadHost(e.cache / "Save.DAT") == "hell");
  CHECK(Create("cache:\\save.dat", kGenericWrite, kCreate, 0).status == kStatusObjectNameCollision);
  auto ow = Create("cache:\\SAVE.dat", kGenericWrite, kOverwriteIf, kFileSynchronousIoNonalert);
  REQUIRE(ow.status == kStatusSuccess);
  Call(__imp__NtClose, {ow.handle});
  CHECK(ReadHost(e.cache / "Save.DAT") == "");
  // directory created on cache:, then a file inside it
  auto d = Create("cache:\\Profiles", kGenericRead, kOpenIf, kFileDirectoryFile);
  REQUIRE(d.status == kStatusSuccess);
  Call(__imp__NtClose, {d.handle});
  CHECK(fs::is_directory(e.cache / "Profiles"));
  auto f = Create("cache:\\profiles\\p.bin", kGenericWrite, kCreate, 0);
  CHECK(f.status == kStatusSuccess);
  Call(__imp__NtClose, {f.handle});
  CHECK(fs::exists(e.cache / "Profiles" / "p.bin"));
  // a just-created file is found immediately, with any case
  auto again = Open("cache:\\PROFILES\\P.BIN");
  CHECK(again.status == kStatusSuccess);
  Call(__imp__NtClose, {again.handle});
}

TEST_CASE("file: symbolic links and IoDismountVolumeByName") {
  Env& e = E();
  e.Mount();
  CHECK(Call(__imp__ObCreateSymbolicLink, {e.Ansi("\\??\\media:"), e.Ansi("\\Device\\Cdrom0\\GearGame")}) ==
        kStatusSuccess);
  auto o = Open("media:\\CookedXenon\\Startup.xxx");
  CHECK(o.status == kStatusSuccess);
  Call(__imp__NtClose, {o.handle});
  CHECK(Open("media:\\..\\..\\outside\\secret.txt").status == kStatusObjectNameInvalid);
  CHECK(Call(__imp__ObDeleteSymbolicLink, {e.Ansi("\\??\\media:")}) == kStatusSuccess);
  CHECK(Open("media:\\CookedXenon\\Startup.xxx").status == kStatusObjectPathNotFound);
  CHECK(Call(__imp__ObDeleteSymbolicLink, {e.Ansi("\\??\\media:")}) != kStatusSuccess);
  CHECK(Call(__imp__ObCreateSymbolicLink, {0, e.Ansi("game:")}) == kStatusInvalidParameter);
  CHECK(Call(__imp__IoDismountVolumeByName, {e.Ansi("\\Device\\Harddisk0\\Partition1")}) == kStatusSuccess);
}

TEST_CASE("file: lengths and ANSI_STRING beyond 4 GiB rejected without touching host memory") {
  Env& e = E();
  e.Mount();
  auto o = Open("game:\\Readme.TXT");
  REQUIRE(o.status == kStatusSuccess);
  const uint32_t iosb = e.Alloc(8);
  // info at the end of the guest space with a length that exceeds it: no memset
  CHECK(Call(__imp__NtQueryInformationFile, {o.handle, iosb, 0xFFFFFF00, 0x1000, kFileNetworkOpenInformation}) ==
        kStatusAccessViolation);
  CHECK(Call(__imp__NtQueryVolumeInformationFile, {o.handle, iosb, 0xFFFFFF00, 0x1000, kFileFsSizeInformation}) ==
        kStatusAccessViolation);
  // ANSI_STRING with the buffer straddling the end of the guest space
  const uint32_t bad = e.Alloc(8);
  *GuestPtr<be<uint16_t>>(bad) = uint16_t(0x100);
  *GuestPtr<be<uint16_t>>(bad + 2) = uint16_t(0x100);
  Put32(bad + 4, 0xFFFFFFF0);
  CHECK(Call(__imp__ObCreateSymbolicLink, {bad, e.Ansi("game:")}) == kStatusInvalidParameter);
  CHECK(Call(__imp__ObDeleteSymbolicLink, {bad}) == kStatusInvalidParameter);
  // ANSI_STRING itself straddling the end
  CHECK(Call(__imp__ObDeleteSymbolicLink, {0xFFFFFFFC}) == kStatusInvalidParameter);
  const uint32_t oa = e.Alloc(12);
  Put32(oa, 0);
  Put32(oa + 4, bad);
  Put32(oa + 8, 0x40);
  const uint32_t out = e.Alloc(4);
  CHECK(Call(__imp__NtCreateFile, {out, kGenericRead, oa, iosb, 0, 0, 0, kOpen, 0}) == kStatusObjectNameInvalid);
  Call(__imp__NtClose, {o.handle});
}

TEST_CASE("file: count of opened packages") {
  Env& e = E();
  e.Mount();
  const uint64_t packages = SummaryValue("packages_opened");
  const uint64_t cooked = SummaryValue("cooked_packages_opened");
  REQUIRE(packages != ~0ull);
  REQUIRE(cooked != ~0ull);
  auto o = Open("game:\\geargame\\cookedxenon\\startup.XXX");
  REQUIRE(o.status == kStatusSuccess);
  Call(__imp__NtClose, {o.handle});
  o = Open("\\Device\\Cdrom0\\GearGame\\Movies\\Intro.upk");
  REQUIRE(o.status == kStatusSuccess);
  Call(__imp__NtClose, {o.handle});
  o = Open("game:\\Readme.TXT");  // not a package
  Call(__imp__NtClose, {o.handle});
  Open("game:\\GearGame\\CookedXenon\\Missing.xxx");  // not opened: does not count
  CHECK(SummaryValue("packages_opened") == packages + 2);
  CHECK(SummaryValue("cooked_packages_opened") == cooked + 1);
  // the recorded name is the normalized one, with the host's case
  CHECK(stats::Summary().find("[summary] package game:\\GearGame\\CookedXenon\\Startup.xxx\n") != std::string::npos);
}

TEST_CASE("file: FscSetCacheElementCount succeeds with no effects") {
  CHECK(Call(__imp__FscSetCacheElementCount, {0, 0x20}) == 0);
}

// The game often reads straight into physical memory (textures, vertices) that
// the GPU watches: read-only pages in the 0xA/0xC/0xE views. The file read
// must write the data and invalidate the GPU (physwatch callback)
// like a CPU write; an OS ReadFile writing directly
// into the protected pages would fail (ERROR_NOACCESS) with no exception.
namespace {
struct ReadWatch {
  std::atomic<int> count{0};
  std::atomic<uint32_t> first{~0u}, last{0};
};
ReadWatch g_read_watch;
std::pair<uint32_t, uint32_t> RecordReadWatch(void* ctx, uint32_t phys, uint32_t len, bool) {
  auto* w = static_cast<ReadWatch*>(ctx);
  ++w->count;
  w->first = std::min(w->first.load(), phys);
  w->last = std::max(w->last.load(), phys + len);
  return {phys, len};
}
}  // namespace

TEST_CASE("file: NtReadFile into physical pages watched by the GPU invalidates and writes the data") {
  Env& e = E();
  e.Mount();
  // ~254 KiB: much more than the stream's buffer
  std::string content(0x3F800, '\0');
  for (size_t i = 0; i < content.size(); ++i) content[i] = char('a' + (i * 7 + i / 251) % 26);
  Touch(e.game / "Phys.bin", content);
  auto o = Open("game:\\Phys.bin");
  REQUIRE(o.status == kStatusSuccess);

  physwatch::Init(e.mem);
  g_read_watch.count = 0;
  // Cleanup even if a REQUIRE fails: callback removed, block freed,
  // arena detached (the process's other tests are not watched), file closed.
  struct Cleanup {
    GuestMemory& mem;
    uint32_t file;
    void* handle = nullptr;
    uint32_t block = 0;
    ~Cleanup() {
      if (handle) physwatch::Unregister(handle);
      if (block) Memory()->phys.Free(block);
      physwatch::Detach(mem);
      Call(__imp__NtClose, {file});
    }
  } cleanup{e.mem, o.handle};
  cleanup.handle = physwatch::Register(&RecordReadWatch, &g_read_watch);
  const uint32_t block = Memory()->phys.Alloc(0x41000, 0x1000);
  REQUIRE(block != 0);
  cleanup.block = block;
  const uint32_t phys = block & 0x1FFFFFFF;
  physwatch::Watch(phys, 0x41000);
  REQUIRE(physwatch::IsWatched(phys + 0x1000));

  // unaligned destination: from page 0 (offset 0x10) to page 0x3F,
  // from the 0xA view and from 0xE (address of physical p: 0xE0000000 + p - 0x1000)
  const uint32_t view = GENERATE(0xA0000000u, 0xE0000000u);
  INFO("view " << std::hex << view);
  const uint32_t dst = (view == 0xE0000000u ? view + phys - 0x1000 : block) + 0x10;
  const uint32_t iosb = e.Alloc(8);
  const uint32_t off = e.Alloc(8);
  Put64(off, 0);
  const uint32_t st =
      Call(__imp__NtReadFile, {o.handle, 0, 0, 0, iosb, dst, uint32_t(content.size()), off});
  CHECK(st == kStatusSuccess);
  CHECK(Get32(iosb) == kStatusSuccess);
  CHECK(Get32(iosb + 4) == content.size());
  CHECK(GetStr(dst, content.size()) == content);
  // callback for the written pages (0..0x3F), then no longer watched; 0x40 stays watched
  INFO("callback: " << g_read_watch.count.load());  // one per page: user-mode copy (VEH)
  CHECK(g_read_watch.count == 1);  // a single Trigger on the whole range, before the copy
  CHECK(g_read_watch.first <= phys);
  CHECK(g_read_watch.last >= phys + 0x40000);
  for (uint32_t page = 0; page < 0x40; ++page) CHECK_FALSE(physwatch::IsWatched(phys + page * 0x1000));
  CHECK(physwatch::IsWatched(phys + 0x40000));
}
