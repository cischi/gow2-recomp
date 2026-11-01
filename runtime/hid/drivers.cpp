// See drivers.h.
#include "hid/drivers.h"

#include <array>
#include <atomic>
#include <thread>

#include "kernel/stats.h"
#include "platform/platform.h"

namespace hid {
namespace {

using Clock = std::chrono::steady_clock;

// ---- script ----------------------------------------------------------------

class ScriptDriver final : public InputDriver {
 public:
  ScriptDriver(std::vector<ScriptEvent> events, Clock::time_point start)
      : events_(std::move(events)), seen_(std::make_unique<std::atomic<bool>[]>(events_.size())), start_(start) {}

  std::optional<Gamepad> GetState(uint32_t user) override {
    if (user != 0) return std::nullopt;
    const auto elapsed = Clock::now() - start_;
    if (elapsed < Clock::duration::zero()) return Gamepad{};
    const uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    Gamepad pad;
    for (size_t i = 0; i < events_.size(); ++i) {
      const ScriptEvent& e = events_[i];
      if (ms < e.at_ms || ms >= uint64_t(e.at_ms) + e.hold_ms) continue;
      pad.buttons |= e.buttons;
      if (e.lx) pad.thumb_lx = e.lx;
      if (e.ly) pad.thumb_ly = e.ly;
      if (e.lt) pad.left_trigger = e.lt;
      if (e.rt) pad.right_trigger = e.rt;
      if (e.rx) pad.thumb_rx = e.rx;
      if (e.ry) pad.thumb_ry = e.ry;
      if (!seen_[i].exchange(true, std::memory_order_relaxed)) kernel::stats::NoteInputInjectedEvent();
    }
    return pad;
  }

  bool SetVibration(uint32_t user, uint16_t, uint16_t) override { return user == 0; }

 private:
  const std::vector<ScriptEvent> events_;
  std::unique_ptr<std::atomic<bool>[]> seen_;
  const Clock::time_point start_;
};

// ---- XInput ----------------------------------------------------------------

constexpr uint32_t kUsers = 4;
constexpr uint32_t kPollMicroseconds = 4000;
constexpr auto kReprobeDisconnected = std::chrono::seconds(1);

// State of one user, written only by the polling thread and read by anyone:
// seqlock (odd sequence = write in progress) over two 64-bit words.
class PadSlot {
 public:
  void Store(const std::optional<plat::PadState>& s) {
    uint64_t a = 0, b = 0;
    if (s) {
      a = 1ull << 63 | uint64_t(s->buttons) | uint64_t(s->lt) << 16 | uint64_t(s->rt) << 24 |
          uint64_t(uint16_t(s->lx)) << 32;
      b = uint64_t(uint16_t(s->ly)) | uint64_t(uint16_t(s->rx)) << 16 | uint64_t(uint16_t(s->ry)) << 32;
    }
    if (a == a_.load(std::memory_order_relaxed) && b == b_.load(std::memory_order_relaxed)) return;
    const uint32_t seq = seq_.load(std::memory_order_relaxed);
    seq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    a_.store(a, std::memory_order_relaxed);
    b_.store(b, std::memory_order_relaxed);
    seq_.store(seq + 2, std::memory_order_release);
  }

  std::optional<Gamepad> Load() const {
    uint64_t a, b;
    while (true) {
      const uint32_t before = seq_.load(std::memory_order_acquire);
      a = a_.load(std::memory_order_relaxed);
      b = b_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (!(before & 1) && seq_.load(std::memory_order_relaxed) == before) break;
    }
    if (!(a >> 63)) return std::nullopt;
    Gamepad g;
    g.buttons = uint16_t(a);
    g.left_trigger = uint8_t(a >> 16);
    g.right_trigger = uint8_t(a >> 24);
    g.thumb_lx = int16_t(uint16_t(a >> 32));
    g.thumb_ly = int16_t(uint16_t(b));
    g.thumb_rx = int16_t(uint16_t(b >> 16));
    g.thumb_ry = int16_t(uint16_t(b >> 32));
    return g;
  }

  bool Connected() const { return a_.load(std::memory_order_relaxed) >> 63; }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint64_t> a_{0}, b_{0};
};

class XInputDriver final : public InputDriver {
 public:
  XInputDriver() {
    PollAll(/*force_probe=*/true);  // valid state already at the first GetState
    thread_ = std::thread([this] {
      plat::SetCurrentThreadName("gow2 xinput");
      while (!stop_.load(std::memory_order_relaxed)) {
        plat::SleepPrecise(kPollMicroseconds);
        PollAll(false);
      }
    });
  }
  ~XInputDriver() override {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  std::optional<Gamepad> GetState(uint32_t user) override {
    if (user >= kUsers) return std::nullopt;
    return slots_[user].Load();
  }

  bool SetVibration(uint32_t user, uint16_t left, uint16_t right) override {
    if (user >= kUsers || !slots_[user].Connected()) return false;
    return plat::XInputSetVibration(user, left, right);
  }

 private:
  // Polling thread (and the constructor, before it starts).
  void PollAll(bool force_probe) {
    const auto now = Clock::now();
    for (uint32_t u = 0; u < kUsers; ++u) {
      if (!force_probe && !slots_[u].Connected() && now - last_probe_[u] < kReprobeDisconnected) continue;
      last_probe_[u] = now;
      slots_[u].Store(plat::XInputGetState(u));
    }
  }

  std::array<PadSlot, kUsers> slots_;
  std::array<Clock::time_point, kUsers> last_probe_{};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// ---- keyboard --------------------------------------------------------------

// Windows virtual-key codes (VK_*), without windows.h.
constexpr uint8_t kVkReturn = 0x0D, kVkEscape = 0x1B, kVkSpace = 0x20, kVkLeft = 0x25, kVkUp = 0x26,
                  kVkRight = 0x27, kVkDown = 0x28;

class KeyboardDriver final : public InputDriver {
 public:
  explicit KeyboardDriver(plat::Window* window) : window_(window) {}

  std::optional<Gamepad> GetState(uint32_t user) override {
    if (user != 0) return std::nullopt;
    Gamepad pad;
    if (!window_ || !plat::HasFocus(window_)) return pad;
    auto down = [&](uint8_t vk) { return plat::IsKeyDown(window_, vk); };
    if (down(kVkReturn)) pad.buttons |= kButtonStart;
    if (down(kVkEscape)) pad.buttons |= kButtonBack;
    if (down(kVkSpace)) pad.buttons |= kButtonA;
    if (down(kVkUp)) pad.buttons |= kButtonDpadUp;
    if (down(kVkDown)) pad.buttons |= kButtonDpadDown;
    if (down(kVkLeft)) pad.buttons |= kButtonDpadLeft;
    if (down(kVkRight)) pad.buttons |= kButtonDpadRight;
    pad.thumb_ly = int16_t((down('W') ? 32767 : 0) - (down('S') ? 32767 : 0));
    pad.thumb_lx = int16_t((down('D') ? 32767 : 0) - (down('A') ? 32767 : 0));
    return pad;
  }

  bool SetVibration(uint32_t user, uint16_t, uint16_t) override { return user == 0; }

 private:
  plat::Window* const window_;
};

}  // namespace

std::unique_ptr<InputDriver> CreateScriptDriver(std::vector<ScriptEvent> events, Clock::time_point start) {
  return std::make_unique<ScriptDriver>(std::move(events), start);
}

std::unique_ptr<InputDriver> CreateXInputDriver() {
  if (!plat::XInputAvailable()) return nullptr;
  return std::make_unique<XInputDriver>();
}

std::unique_ptr<InputDriver> CreateKeyboardDriver(plat::Window* window) {
  return std::make_unique<KeyboardDriver>(window);
}

}  // namespace hid
