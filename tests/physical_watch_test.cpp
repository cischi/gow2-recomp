// Watching of physical pages (runtime/physical_watch): read-only Watch
// on the three views, write access violations handled by the vectored
// handler of runtime/platform, invalidation callbacks like Xenia's
// PhysicalHeap::TriggerCallbacks, the kernel's physical free/allocation.
// On a real GuestMemory arena with the kernel's allocators.
#include "physical_watch.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "guest_memory.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/registry.h"
#include "platform/platform.h"
#include "temp_dir.h"

PPC_EXTERN_FUNC(__imp__MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__MmFreePhysicalMemory);

using namespace std::chrono_literals;

namespace {

constexpr uint32_t kPage = 0x1000;

// Record of the callback calls: fixed array and atomics, the callbacks
// run in the exception handler.
struct Calls {
  static constexpr int kMax = 64;
  std::atomic<int> count{0};
  uint32_t phys[kMax] = {};
  uint32_t len[kMax] = {};
  bool exact[kMax] = {};
  void Reset() { count = 0; }
};
Calls g_calls;
// Range returned by the "closed" callback (CallbackExact): the one received.
std::pair<uint32_t, uint32_t> RecordCallback(void* ctx, uint32_t phys, uint32_t len, bool exact) {
  auto* calls = static_cast<Calls*>(ctx);
  const int i = calls->count.fetch_add(1);
  if (i < Calls::kMax) {
    calls->phys[i] = phys;
    calls->len[i] = len;
    calls->exact[i] = exact;
  }
  return {phys, len};
}

// Arena + kernel allocators + physwatch attached; a callback that
// records into g_calls.
struct Env {
  GuestMemory mem;
  void* handle = nullptr;
  Env() {
    REQUIRE(mem.Init());
    kernel::g_base = mem.base;
    REQUIRE(kernel::InitMemory(mem));
    physwatch::Init(mem);
    g_calls.Reset();
    handle = physwatch::Register(&RecordCallback, &g_calls);
  }
  ~Env() {
    physwatch::Unregister(handle);
    kernel::ShutdownMemory();
  }
  // Host byte of the view: base + offset, without the guest shift of the
  // 0xE view (physwatch works on the views' host pages).
  volatile uint8_t* Host(uint32_t guest) { return mem.base + guest; }
  uint32_t Call(PPCFunc* fn, std::initializer_list<uint64_t> args) {
    PPCContext ctx{};
    uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64,
                         &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, mem.base);
    return ctx.r3.u32;
  }
};

}  // namespace

TEST_CASE("physical_watch: write to a watched page from each mirror", "[physical_watch]") {
  Env env;
  physwatch::Watch(0x10000, 0x2000);
  CHECK(physwatch::IsWatched(0x10000));
  CHECK(physwatch::IsWatched(0x11FFF));
  CHECK_FALSE(physwatch::IsWatched(0x12000));
  // read-only: reading calls nothing
  CHECK(env.Host(0xA0010800)[0] == 0);
  CHECK(env.Host(0xC0011000)[0] == 0);
  CHECK(g_calls.count == 0);

  SECTION("via 0xA0010800") {
    env.Host(0xA0010800)[0] = 0x5A;
    REQUIRE(g_calls.count == 1);
    CHECK(g_calls.phys[0] == 0x10000);
    CHECK(g_calls.len[0] >= 0x1000);
    CHECK_FALSE(g_calls.exact[0]);
    CHECK(env.Host(0xE0010800)[0] == 0x5A);  // the write took place
    CHECK_FALSE(physwatch::IsWatched(0x10000));
    env.Host(0xA0010801)[0] = 0x5B;  // no longer watched: no callback
    CHECK(g_calls.count == 1);
    env.Host(0xC0010802)[0] = 0x5C;  // reopened in the other mirrors too
    env.Host(0xE0010803)[0] = 0x5D;
    CHECK(g_calls.count == 1);
    CHECK(physwatch::IsWatched(0x11000));  // the other page stays watched
  }
  SECTION("via 0xC0011000") {
    env.Host(0xC0011000)[0] = 0x6A;
    REQUIRE(g_calls.count == 1);
    CHECK(g_calls.phys[0] == 0x11000);
    CHECK(g_calls.len[0] >= 0x1000);
    CHECK(env.Host(0xA0011000)[0] == 0x6A);
    env.Host(0xC0011001)[0] = 0x6B;
    env.Host(0xA0011002)[0] = 0x6C;
    CHECK(g_calls.count == 1);
  }
  SECTION("via 0xE0011000") {
    env.Host(0xE0011000)[0] = 0x7A;
    REQUIRE(g_calls.count == 1);
    CHECK(g_calls.phys[0] == 0x11000);
    CHECK(g_calls.len[0] >= 0x1000);
    CHECK(env.Host(0xC0011000)[0] == 0x7A);
    env.Host(0xE0011001)[0] = 0x7B;
    CHECK(g_calls.count == 1);
  }
}

TEST_CASE("physical_watch: host memcpy onto a watched page", "[physical_watch]") {
  Env env;
  physwatch::Watch(0x20000, 0x1000);
  uint8_t src[256];
  for (int i = 0; i < 256; ++i) src[i] = uint8_t(i ^ 0x3C);
  std::memcpy(const_cast<uint8_t*>(env.Host(0xA0020100)), src, sizeof(src));
  REQUIRE(g_calls.count == 1);
  CHECK(g_calls.phys[0] == 0x20000);
  CHECK(std::memcmp(const_cast<uint8_t*>(env.Host(0xE0020100)), src, sizeof(src)) == 0);
}

TEST_CASE("physical_watch: 8-byte write straddling two watched pages", "[physical_watch]") {
  Env env;
  physwatch::Watch(0x30000, 0x2000);
  auto* p = reinterpret_cast<volatile uint64_t*>(env.Host(0xA0030FFC));
  *p = 0x1122334455667788ull;
  CHECK(*reinterpret_cast<volatile uint64_t*>(env.Host(0xC0030FFC)) == 0x1122334455667788ull);
  CHECK_FALSE(physwatch::IsWatched(0x30000));
  CHECK_FALSE(physwatch::IsWatched(0x31000));
  // both pages invalidated (one callback per page, or one for both)
  bool page0 = false, page1 = false;
  for (int i = 0; i < g_calls.count; ++i) {
    const uint64_t b = g_calls.phys[i], e = b + g_calls.len[i];
    page0 |= b <= 0x30000 && e > 0x30000;
    page1 |= b <= 0x31000 && e > 0x31000;
  }
  CHECK(page0);
  CHECK(page1);
}

TEST_CASE("physical_watch: MmFreePhysicalMemory invalidates, the reallocation is zeroed and unwatched",
          "[physical_watch]") {
  Env env;
  const uint32_t a = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x20000, 0x4, 0, 0xFFFFFFFF, 0});
  REQUIRE(a != 0);
  // 4 KB pages: address in the 0xE view, physical with the 0x1000 shift;
  // Host() takes host offsets, here those of the 0xA view
  const uint32_t phys = env.Call(__imp__MmGetPhysicalAddress, {a});
  REQUIRE(phys == (a & 0x1FFFFFFF) + 0x1000);
  const uint32_t pa = 0xA0000000u | phys;
  std::memset(const_cast<uint8_t*>(env.Host(pa)), 0xAB, 0x20000);
  physwatch::Watch(phys, 0x20000);
  g_calls.Reset();
  env.Call(__imp__MmFreePhysicalMemory, {0, a});
  // callback before the return, on the freed range
  REQUIRE(g_calls.count >= 1);
  CHECK(g_calls.phys[0] <= phys);
  CHECK(uint64_t(g_calls.phys[0]) + g_calls.len[0] >= uint64_t(phys) + 0x20000);
  CHECK_FALSE(physwatch::IsWatched(phys));
  CHECK_FALSE(physwatch::IsWatched(phys + 0x1F000));
  // the freed memory stays committed and readable from all mirrors
  (void)env.Host(pa)[0];
  (void)env.Host(0xC0000000u | phys)[0x1FFFF];
  (void)env.Host(0xE0000000u | phys)[0x10000];

  const uint32_t b = env.Call(__imp__MmAllocatePhysicalMemoryEx, {0, 0x20000, 0x4, 0, 0xFFFFFFFF, 0});
  REQUIRE(b == a);
  for (uint32_t off = 0; off < 0x20000; off += 0x7F1) CHECK(env.Host(pa)[off] == 0);
  CHECK(env.Host(0xE0000000u | phys)[0x1FFFF] == 0);
  g_calls.Reset();
  env.Host(pa)[0x100] = 1;
  env.Host(0xC0000000u | phys)[0x1F000] = 2;
  CHECK(g_calls.count == 0);
  env.Call(__imp__MmFreePhysicalMemory, {0, b});
}

TEST_CASE("physical_watch: Commit and Decommit in the 0xE view invalidate the physical shifted by 0x1000",
          "[physical_watch]") {
  Env env;
  for (uint32_t phys : {0x4000u, 0x5000u}) {
    physwatch::Watch(phys, kPage);
    REQUIRE(physwatch::IsWatched(phys));
  }
  g_calls.Reset();
  REQUIRE(env.mem.Decommit(0xE0004000u, kPage));
  REQUIRE(g_calls.count == 1);
  CHECK(g_calls.phys[0] == 0x5000u);
  CHECK_FALSE(physwatch::IsWatched(0x5000));
  CHECK(physwatch::IsWatched(0x4000));

  physwatch::Watch(0x5000, kPage);
  g_calls.Reset();
  REQUIRE(env.mem.Commit(0xE0004000u, kPage));
  REQUIRE(g_calls.count == 1);
  CHECK(g_calls.phys[0] == 0x5000u);
  CHECK_FALSE(physwatch::IsWatched(0x5000));
  CHECK(physwatch::IsWatched(0x4000));
}

TEST_CASE("physical_watch: range to stop watching = intersection of the callbacks, max 4 MiB",
          "[physical_watch]") {
  Env env;  // Env's callback returns the range received (the fault's page)
  struct Wide {
    static std::pair<uint32_t, uint32_t> Big(void*, uint32_t, uint32_t, bool) { return {0, 0x1000000}; }
    static std::pair<uint32_t, uint32_t> Mid(void*, uint32_t, uint32_t, bool) { return {0x300000, 0x200000}; }
  };
  physwatch::Unregister(env.handle);
  void* big = physwatch::Register(&Wide::Big, nullptr);
  void* mid = physwatch::Register(&Wide::Mid, nullptr);
  physwatch::Watch(0x200000, 0x600000);  // [2 MiB, 8 MiB)

  SECTION("inexact: [3 MiB, 5 MiB) intersected with the page's 4 MiB block -> [3 MiB, 4 MiB)") {
    CHECK(physwatch::Trigger(0x380000, 1, false));
    CHECK(physwatch::IsWatched(0x2FF000));
    CHECK_FALSE(physwatch::IsWatched(0x300000));
    CHECK_FALSE(physwatch::IsWatched(0x3FF000));
    CHECK(physwatch::IsWatched(0x400000));
    // really reopened: writing does not fault
    env.Host(0xA0300000)[0] = 1;
    env.Host(0xE03FF000)[0] = 1;
  }
  SECTION("exact: only the requested page") {
    CHECK(physwatch::Trigger(0x380000, 1, true));
    CHECK(physwatch::IsWatched(0x37F000));
    CHECK_FALSE(physwatch::IsWatched(0x380000));
    CHECK(physwatch::IsWatched(0x381000));
  }
  SECTION("no watched page: false, no effect") {
    CHECK_FALSE(physwatch::Trigger(0x100000, 0x1000, false));
    CHECK(physwatch::IsWatched(0x200000));
  }
  physwatch::Unregister(big);
  physwatch::Unregister(mid);
  env.handle = physwatch::Register(&RecordCallback, &g_calls);
}

TEST_CASE("physical_watch: fault while another thread holds GlobalLock", "[physical_watch]") {
  Env env;
  physwatch::Watch(0x40000, 0x1000);
  std::atomic<bool> locked{false};
  std::atomic<int64_t> released_at{0};
  const auto t0 = std::chrono::steady_clock::now();
  const auto since = [t0] {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  };
  std::thread holder([&] {
    std::lock_guard lock(physwatch::GlobalLock());
    locked = true;
    std::this_thread::sleep_for(50ms);
    released_at = since();
  });
  while (!locked) std::this_thread::yield();
  auto writer = std::async(std::launch::async, [&] {
    env.Host(0xA0040010)[0] = 0x42;
    return since();
  });
  if (writer.wait_for(2s) != std::future_status::ready) {
    FAIL_CHECK("deadlock: the write does not finish within 2 s");
    std::_Exit(1);
  }
  holder.join();
  CHECK(writer.get() >= released_at.load());
  CHECK(g_calls.count == 1);
  CHECK(env.Host(0xC0040010)[0] == 0x42);
}

TEST_CASE("physical_watch: whoever holds GlobalLock writes to a watched page (recursive)", "[physical_watch]") {
  Env env;
  physwatch::Watch(0x50000, 0x1000);
  auto writer = std::async(std::launch::async, [&] {
    std::lock_guard lock(physwatch::GlobalLock());
    env.Host(0xA0050000)[0] = 0x24;
  });
  if (writer.wait_for(2s) != std::future_status::ready) {
    FAIL_CHECK("deadlock with the recursive lock");
    std::_Exit(1);
  }
  writer.get();
  CHECK(g_calls.count == 1);
  CHECK(env.Host(0xE0050000)[0] == 0x24);
}

TEST_CASE("physical_watch: 8 threads on 8 watched pages, no callback lost", "[physical_watch]") {
  Env env;
  physwatch::Watch(0x60000, 8 * kPage);
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < 8; ++i)
    threads.emplace_back([&, i] {
      while (!go) std::this_thread::yield();
      env.Host(0xA0060000 + i * kPage + 4 * i)[0] = uint8_t(0x80 + i);
    });
  go = true;
  for (auto& t : threads) t.join();
  REQUIRE(g_calls.count == 8);
  bool seen[8] = {};
  for (int i = 0; i < 8; ++i) {
    REQUIRE(g_calls.phys[i] >= 0x60000);
    REQUIRE(g_calls.phys[i] < 0x68000);
    seen[(g_calls.phys[i] - 0x60000) / kPage] = true;
  }
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(seen[i]);
    CHECK(env.Host(0xC0060000 + i * kPage + 4 * i)[0] == uint8_t(0x80 + i));
  }
}

TEST_CASE("physical_watch: Unregister waits for the callback in progress and removes it", "[physical_watch]") {
  Env env;
  struct Slow {
    std::atomic<bool> inside{false}, done{false};
    std::atomic<int> calls{0};
    static std::pair<uint32_t, uint32_t> Cb(void* ctx, uint32_t phys, uint32_t len, bool) {
      auto* s = static_cast<Slow*>(ctx);
      s->calls.fetch_add(1);
      s->inside = true;
      std::this_thread::sleep_for(50ms);
      s->done = true;
      return {phys, len};
    }
  };
  Slow slow;
  void* h = physwatch::Register(&Slow::Cb, &slow);
  physwatch::Watch(0x70000, 0x2000);
  std::thread writer([&] { env.Host(0xA0070000)[0] = 1; });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!slow.inside && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  if (!slow.inside) {
    FAIL_CHECK("the callback did not start within 2 s");
    std::_Exit(1);  // the writing thread might be blocked
  }
  physwatch::Unregister(h);
  CHECK(slow.done);  // Unregister returned only after the callback
  writer.join();
  env.Host(0xA0071000)[0] = 1;  // second page: the removed callback is not called
  CHECK(slow.calls == 1);
  CHECK(g_calls.count == 2);
}

TEST_CASE("physical_watch: without an attached arena Watch does nothing; a destroyed GuestMemory detaches",
          "[physical_watch]") {
  {
    GuestMemory m;
    REQUIRE(m.Init());
    physwatch::Init(m);
    CHECK(physwatch::IsAttached(m));
    physwatch::Watch(0x80000, 0x1000);
    CHECK(physwatch::IsWatched(0x80000));
  }
  CHECK_FALSE(physwatch::IsWatched(0x80000));
  physwatch::Watch(0x80000, 0x1000);
  CHECK_FALSE(physwatch::IsWatched(0x80000));
  CHECK_FALSE(physwatch::OnWriteFault(0x123456789ull));
}

TEST_CASE("physical_watch: physical memory is committed and readable even if never allocated", "[physical_watch]") {
  GuestMemory m;
  REQUIRE(m.Init());
  CHECK(plat::IsWritable(m.base + 0xA0000000));
  CHECK(plat::IsWritable(m.base + 0xDFFFF000));
  CHECK(plat::IsWritable(m.base + 0xFFFFF000));
  CHECK(m.base[0xBFD40000] == 0);
  CHECK(m.base[0xFFFFFFFF] == 0);
}

TEST_CASE("physical_watch: an unwatched access violation reaches the crash handler (exit 3)",
          "[physical_watch]") {
  const auto log = gow2_test::UniqueTempPath("gow2_physwatch_crash_child");
  const std::string cmd =
      "\"\"" + std::string(GOW2_PHYSWATCH_CRASH_CHILD) + "\" \"" + log.string() + "\" >nul 2>&1\"";
  const int rc = std::system(cmd.c_str());
  std::ifstream in(log);
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  std::filesystem::remove(log);
  INFO(ss.str());
  CHECK(rc == 3);
  CHECK(ss.str().find("watched ok") != std::string::npos);  // the VEH was active
  CHECK(ss.str().find("crash host=") != std::string::npos);
  CHECK(ss.str().find("code=0xC0000005") != std::string::npos);
  CHECK(ss.str().find("no crash") == std::string::npos);
}
