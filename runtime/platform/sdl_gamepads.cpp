// Non-Xbox pads with SDL3 (see sdl_gamepads.h). SDL (thirdparty/SDL @
// release-3.4.18, zlib) is configured in cmake/SDL.cmake with only the
// joystick/gamepad subsystem + HIDAPI.
#include "platform/sdl_gamepads.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "platform/platform.h"

namespace plat {
namespace {

constexpr uint32_t kPollMicroseconds = 4000;
constexpr size_t kMaxPads = 8;

// One instance at a time: SDL_Init/SDL_Quit are process-global.
std::atomic<bool> g_running{false};

static_assert(sizeof(GamepadSnapshot) % sizeof(uint64_t) == 0);
static_assert(std::is_trivially_copyable_v<GamepadSnapshot>);
constexpr size_t kWordsPerPad = sizeof(GamepadSnapshot) / sizeof(uint64_t);

// Snapshot of all pads written only by the SDL thread and read by anyone:
// seqlock (odd sequence = write in progress) over atomic words, so
// the concurrent copy is not a data race.
class SnapshotCache {
 public:
  void Store(std::span<const GamepadSnapshot> pads) {
    std::array<uint64_t, kMaxPads * kWordsPerPad> words{};
    const size_t n = std::min(pads.size(), kMaxPads);
    if (n) std::memcpy(words.data(), pads.data(), n * sizeof(GamepadSnapshot));
    const uint32_t seq = seq_.load(std::memory_order_relaxed);
    seq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    count_.store(n, std::memory_order_relaxed);
    for (size_t i = 0; i < n * kWordsPerPad; ++i) words_[i].store(words[i], std::memory_order_relaxed);
    seq_.store(seq + 2, std::memory_order_release);
  }

  size_t Load(std::span<GamepadSnapshot> out) const {
    std::array<uint64_t, kMaxPads * kWordsPerPad> words;
    size_t n;
    while (true) {
      const uint32_t before = seq_.load(std::memory_order_acquire);
      n = std::min(count_.load(std::memory_order_relaxed), std::min(out.size(), kMaxPads));
      for (size_t i = 0; i < n * kWordsPerPad; ++i) words[i] = words_[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (!(before & 1) && seq_.load(std::memory_order_relaxed) == before) break;
    }
    if (n) std::memcpy(out.data(), words.data(), n * sizeof(GamepadSnapshot));
    return n;
  }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<size_t> count_{0};
  std::array<std::atomic<uint64_t>, kMaxPads * kWordsPerPad> words_{};
};

bool IsXbox(SDL_GamepadType t) { return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE; }

const char* TypeName(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_STANDARD: return "Standard";
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One";
    case SDL_GAMEPAD_TYPE_PS3: return "PS3";
    case SDL_GAMEPAD_TYPE_PS4: return "PS4";
    case SDL_GAMEPAD_TYPE_PS5: return "PS5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con L";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con R";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return "GameCube";
    case SDL_GAMEPAD_TYPE_STEAM: return "Steam";
    default: return "Unknown";
  }
}

template <size_t N>
void CopyString(char (&dst)[N], const char* src) {
  if (!src) src = "";
  const size_t len = std::min(std::strlen(src), N - 1);
  std::memcpy(dst, src, len);
  std::memset(dst + len, 0, N - len);
}

struct RumbleRequest {
  uint32_t instance_id;
  uint16_t low, high;
};

}  // namespace

namespace {
// Active instance for StopAllRumble (static: the game exits from any
// thread, without going through the owner). The destructor clears it under
// this mutex before stopping the SDL thread.
std::mutex g_instance_mutex;
SdlGamepads::Impl* g_instance = nullptr;  // protected by g_instance_mutex
}  // namespace

struct SdlGamepads::Impl {
  struct Pad {
    SDL_Gamepad* gamepad;
    SDL_JoystickID id;
    uint64_t connect_order;
    const char* type;
  };

  SnapshotCache cache;
  std::atomic<bool> stop{false};
  std::mutex rumble_mutex;
  std::condition_variable rumble_cv;        // signals changes of stop_done
  std::vector<RumbleRequest> rumble_queue;  // protected by rumble_mutex
  uint64_t stop_requested = 0;              // protected by rumble_mutex: StopAllRumble requests
  uint64_t stop_done = 0;                   // protected by rumble_mutex: last one executed
  std::thread thread;

  // ---- SDL thread only ----
  std::vector<Pad> pads;
  uint64_t next_order = 1;

  // Thread body: init, loop, SDL_Quit. `started` receives the result of SDL_Init.
  void Run(std::promise<std::string> started) {
    SetCurrentThreadName("gow2 sdl");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Without this hint SDL_Init (SDL_InitTicks) calls
    // timeBeginPeriod(1): it would change the timer granularity of the whole
    // process, and with it the game's timing. "0" leaves it as it is (this
    // thread's loop uses SleepPrecise anyway).
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    // Xbox pads stay with XInput: no HIDAPI Xbox driver (less work in
    // SDL_Init). SDL's XInput backend is kept active on purpose: it is what
    // recognizes as Xbox 360 (and therefore excludes) even third-party XInput
    // pads with unknown VID/PID, which would otherwise arrive from
    // RawInput/DirectInput as "Standard" and show up twice.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
      std::string error = SDL_GetError();
      if (error.empty()) error = "SDL_Init(SDL_INIT_GAMEPAD) failed";
      SDL_Quit();
      started.set_value(std::move(error));
      return;
    }
    // Pads already connected arrive as SDL_EVENT_GAMEPAD_ADDED on the first loop.
    Poll();
    started.set_value({});
    while (!stop.load(std::memory_order_relaxed)) {
      SleepPrecise(kPollMicroseconds);
      Poll();
    }
    // SDL_CloseGamepad does not stop a vibration with duration 0: clear it first.
    for (Pad& p : pads) {
      SDL_RumbleGamepad(p.gamepad, 0, 0, 0);
      SDL_CloseGamepad(p.gamepad);
    }
    pads.clear();
    cache.Store({});
    SDL_Quit();
    {
      std::lock_guard lock(rumble_mutex);
      stop_done = stop_requested;  // no pad open: nothing vibrates anymore
    }
    rumble_cv.notify_all();
  }

  // SDL_PollEvent pumps the events (SDL_PumpEvents -> SDL_UpdateJoysticks)
  // once per queue drain: the state read in Publish is already
  // up to date, without an extra SDL_UpdateGamepads.
  void Poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
        Add(e.gdevice.which);
      } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        Remove(e.gdevice.which);
      }
    }
    RunRumbles();
    Publish();
  }

  void Add(SDL_JoystickID id) {
    for (const Pad& p : pads)
      if (p.id == id) return;
    const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
    // Xbox according to SDL (also as the real type, under any mappings):
    // XInput already reads it.
    if (IsXbox(type) || IsXbox(SDL_GetRealGamepadTypeForID(id))) return;
    SDL_Gamepad* g = SDL_OpenGamepad(id);
    if (!g) return;
    pads.push_back({g, id, next_order++, TypeName(type)});
  }

  void Remove(SDL_JoystickID id) {
    auto it = std::find_if(pads.begin(), pads.end(), [&](const Pad& p) { return p.id == id; });
    if (it == pads.end()) return;
    SDL_CloseGamepad(it->gamepad);
    pads.erase(it);
  }

  void RunRumbles() {
    std::vector<RumbleRequest> requests;
    uint64_t stop;
    {
      std::lock_guard lock(rumble_mutex);
      requests.swap(rumble_queue);
      stop = stop_requested;
    }
    for (const RumbleRequest& r : requests) {
      for (const Pad& p : pads) {
        // duration 0: the vibration stays until the next command
        if (p.id == r.instance_id) SDL_RumbleGamepad(p.gamepad, r.low, r.high, 0);
      }
    }
    if (stop == stop_applied_) return;
    // StopAllRumble: after the queued requests it clears all open pads,
    // also those without a user.
    for (const Pad& p : pads) SDL_RumbleGamepad(p.gamepad, 0, 0, 0);
    stop_applied_ = stop;
    {
      std::lock_guard lock(rumble_mutex);
      stop_done = stop;
    }
    rumble_cv.notify_all();
  }

  void Publish() {
    // memset: the memcmp comparison below also covers the padding bytes.
    std::array<GamepadSnapshot, kMaxPads> out;
    std::memset(out.data(), 0, sizeof out);
    const size_t n = std::min(pads.size(), kMaxPads);
    for (size_t i = 0; i < n; ++i) {
      const Pad& p = pads[i];
      GamepadSnapshot& s = out[i];
      s.instance_id = p.id;
      s.connect_order = p.connect_order;
      s.buttons = 0;
      for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
        if (SDL_GetGamepadButton(p.gamepad, SDL_GamepadButton(b))) s.buttons |= 1u << b;
      for (int a = 0; a < 6; ++a) s.axes[a] = SDL_GetGamepadAxis(p.gamepad, SDL_GamepadAxis(a));
      CopyString(s.name, SDL_GetGamepadName(p.gamepad));
      CopyString(s.type, p.type);
    }
    if (n == published_count_ && std::memcmp(out.data(), published_.data(), n * sizeof(GamepadSnapshot)) == 0) return;
    std::memcpy(published_.data(), out.data(), sizeof out);
    published_count_ = n;
    cache.Store(std::span(out.data(), n));
  }

 private:
  std::array<GamepadSnapshot, kMaxPads> published_{};  // written only with memcpy (padding included)
  size_t published_count_ = 0;
  uint64_t stop_applied_ = 0;
};

static_assert(SDL_GAMEPAD_AXIS_LEFTX == 0 && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == 5);
static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0 && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

std::unique_ptr<SdlGamepads> SdlGamepads::Start(std::string* error) {
  if (g_running.exchange(true)) {
    if (error) *error = "SDL already started by another SdlGamepads instance";
    return nullptr;
  }
  // If anything throws (allocation, std::thread), g_running goes back to false.
  struct RunningGuard {
    bool armed = true;
    ~RunningGuard() {
      if (armed) g_running.store(false);
    }
  } running_guard;
  auto impl = std::make_unique<Impl>();
  std::promise<std::string> started;
  std::future<std::string> result = started.get_future();
  Impl* raw = impl.get();
  impl->thread = std::thread([raw, p = std::move(started)]() mutable { raw->Run(std::move(p)); });
  std::string message = result.get();
  if (!message.empty()) {
    impl->thread.join();
    if (error) *error = std::move(message);
    return nullptr;
  }
  if (error) error->clear();
  std::unique_ptr<SdlGamepads> pads(new SdlGamepads(std::move(impl)));
  running_guard.armed = false;  // from here on the destructor clears g_running
  {
    std::lock_guard lock(g_instance_mutex);
    g_instance = raw;
  }
  return pads;
}

SdlGamepads::SdlGamepads(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SdlGamepads::~SdlGamepads() {
  {
    std::lock_guard lock(g_instance_mutex);  // waits for a StopAllRumble in progress
    if (g_instance == impl_.get()) g_instance = nullptr;
  }
  impl_->stop.store(true);
  if (impl_->thread.joinable()) impl_->thread.join();
  g_running.store(false);
}

size_t SdlGamepads::Snapshot(std::span<GamepadSnapshot> out) const { return impl_->cache.Load(out); }

void SdlGamepads::Rumble(uint32_t instance_id, uint16_t low, uint16_t high) {
  std::lock_guard lock(impl_->rumble_mutex);
  // Only the last request per pad counts: the queue does not grow without limit.
  for (RumbleRequest& r : impl_->rumble_queue) {
    if (r.instance_id == instance_id) {
      r.low = low;
      r.high = high;
      return;
    }
  }
  impl_->rumble_queue.push_back({instance_id, low, high});
}

bool SdlGamepads::StopAllRumble(std::chrono::milliseconds timeout) {
  std::lock_guard instance_lock(g_instance_mutex);
  Impl* impl = g_instance;
  if (!impl) return true;
  std::unique_lock lock(impl->rumble_mutex);
  impl->rumble_queue.clear();  // the zero on all pads supersedes them
  const uint64_t request = ++impl->stop_requested;
  return impl->rumble_cv.wait_for(lock, timeout, [&] { return impl->stop_done >= request; });
}

int SdlLinkedVersion() { return SDL_GetVersion(); }

}  // namespace plat
