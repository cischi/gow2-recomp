// Audio system (audio_system.h). Xenia references @ 95a5c3ee (BSD):
// src/xenia/apu/audio_system.cc, src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc.
#include "kernel/registry.h"  // first: kernel/function.h (GuestToHostFunction)

#include "apu/audio_system.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "apu/wav_writer.h"
#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/stats.h"
#include "kernel/threads.h"
#include "log.h"

namespace apu {
namespace {

constexpr uint32_t kWorkerStackSize = 128 * 1024;  // same as XHostThread in AudioSystem::Setup

struct Client {
  bool in_use = false;    // slot in use (also during registration)
  bool runnable = false;  // callback installed: the worker may call it
  uint32_t callback = 0;
  uint32_t wrapped_arg = 0;  // 4 bytes of the system heap holding the argument (BE)
  uint32_t credits = 0;
  uint64_t generation = 0;  // of the registration: BufferDone from old sinks does not count
  std::unique_ptr<plat::AudioOut> sink;
};

// Credits returned by the sinks, lock-free (BufferDone runs on the XAudio2
// engine thread, which must never wait for State::mutex: SubmitFrame calls
// Submit -> SubmitSourceBuffer while holding it). One word per slot:
// (generation & kGenMask) << 16 | returned buffers not yet counted.
// The generation and the count change together (CAS), so a BufferDone from a
// previous registration cannot add to the new one.
constexpr uint64_t kGenMask = (uint64_t(1) << 48) - 1;
constexpr uint64_t ReturnedWord(uint64_t generation) { return (generation & kGenMask) << 16; }

struct State {
  std::mutex mutex;
  // Worker wake-up (new credits, new clients, shutdown): atomic counter
  // with wait/notify, also incremented without State::mutex.
  std::atomic<uint32_t> wake{0};
  std::array<std::atomic<uint64_t>, kMaxClients> returned{};
  std::condition_variable idle_cv;  // end of a callback, worker finished, shutdown
  bool initialized = false;
  bool running = false;
  uint64_t epoch = 0;          // Init: a worker from a previous Init exits
  bool worker_exited = true;   // of the worker of the current epoch
  std::shared_ptr<kernel::GuestThread> worker;
  bool worker_failed_logged = false;
  std::array<Client, kMaxClients> clients;
  int in_callback = -1;  // client whose callback the worker is running
  uint32_t next = 0;     // round-robin position over the clients
  uint64_t generations = 0;
  std::vector<uint32_t> deferred_free;  // arguments unregistered inside their own callback
  AudioOptions options;
  SinkFactory factory;
};

State& S() {
  static State s;
  return s;
}

void Wake(State& s) {
  s.wake.fetch_add(1);
  s.wake.notify_all();
}

// Init/Shutdown/worker creation. Order: g_lifecycle -> State::mutex.
std::mutex g_lifecycle;

std::mutex g_capture_mutex;
std::unique_ptr<WavWriter> g_capture;

thread_local bool t_is_worker = false;

void FreeGuest(uint32_t addr) {
  if (!addr) return;
  if (kernel::MemorySystem* m = kernel::Memory()) m->heap.Free(addr);
}

// kXAudio2: XAudio2 2.9 output; if it cannot be created (missing DLL, engine
// or endpoint) a log line and the null sink. Each client has its own sink.
std::unique_ptr<plat::AudioOut> PlatformFactory(plat::AudioSinkKind kind, plat::AudioOut::BufferDone done) {
  if (kind == plat::AudioSinkKind::kXAudio2) {
    uint32_t hr = 0;
    if (auto out = plat::CreateXAudio2AudioOut(done, &hr)) return out;
    char line[96];
    std::snprintf(line, sizeof(line), "[audio] xaudio2 not available (hr=0x%08X), using the null sink", hr);
    LogLine(line);
  }
  return plat::CreateNullAudioOut(std::move(done));
}

// From the sink thread, lock-free: a consumed buffer returns one credit to the
// client of the same registration (counted by the worker, at most
// kClientCredits in total).
void BufferDone(uint32_t index, uint64_t generation) {
  State& s = S();
  std::atomic<uint64_t>& word = s.returned[index];
  uint64_t w = word.load();
  do {
    if ((w >> 16) != (generation & kGenMask) || (w & 0xFFFF) >= kClientCredits) return;
  } while (!word.compare_exchange_weak(w, w + 1));
  Wake(s);
}

// Lock held: adds the buffers returned by the current registration to the credits.
void CollectReturned(State& s) {
  for (uint32_t i = 0; i < kMaxClients; ++i) {
    Client& c = s.clients[i];
    if (!c.in_use) continue;
    std::atomic<uint64_t>& word = s.returned[i];
    uint64_t w = word.load();
    uint32_t n = 0;
    while ((w >> 16) == (c.generation & kGenMask) && (w & 0xFFFF) != 0) {
      if (word.compare_exchange_weak(w, w & ~uint64_t(0xFFFF))) {
        n = uint32_t(w & 0xFFFF);
        break;
      }
    }
    if (n) c.credits = std::min<uint32_t>(kClientCredits, c.credits + n);
  }
}

// Next client with credits, round-robin (lock held). -1 if none.
int PickClient(State& s) {
  CollectReturned(s);
  for (uint32_t k = 0; k < kMaxClients; ++k) {
    const uint32_t i = (s.next + k) % kMaxClients;
    const Client& c = s.clients[i];
    if (c.runnable && c.credits > 0) {
      s.next = (i + 1) % kMaxClients;
      return int(i);
    }
  }
  return -1;
}

// Xenia AudioSystem::WorkerThreadMain: one credit per callback, the game
// callback outside the lock, on the guest thread "Audio Worker".
uint32_t WorkerMain(uint64_t epoch) {
  t_is_worker = true;
  State& s = S();
  std::unique_lock lock(s.mutex);
  while (s.running && s.epoch == epoch) {
    const uint32_t seen = s.wake.load();  // before looking at the credits: no wake-up is lost
    const int index = PickClient(s);
    if (index < 0) {
      lock.unlock();
      s.wake.wait(seen);
      lock.lock();
      continue;
    }
    Client& c = s.clients[index];
    --c.credits;
    s.in_callback = index;
    const uint32_t callback = c.callback, arg = c.wrapped_arg;
    lock.unlock();
    GuestToHostFunction<void>(callback, arg);
    kernel::stats::NoteAudioCallback();
    lock.lock();
    // A worker from a previous Init (left inside the callback past
    // Shutdown) does not touch the current worker's state.
    if (s.epoch != epoch) break;
    s.in_callback = -1;
    if (!s.deferred_free.empty()) {
      std::vector<uint32_t> frees;
      frees.swap(s.deferred_free);
      lock.unlock();
      for (uint32_t a : frees) FreeGuest(a);
      lock.lock();
    }
    s.idle_cv.notify_all();
  }
  if (s.epoch == epoch) s.worker_exited = true;
  s.idle_cv.notify_all();
  return 0;
}

// The worker is created on the first registration (outside the state mutex).
void EnsureWorker() {
  if (t_is_worker) return;
  State& s = S();
  std::lock_guard life(g_lifecycle);
  uint64_t epoch;
  {
    std::lock_guard lock(s.mutex);
    if (!s.running || s.worker) return;
    s.worker_exited = false;
    epoch = s.epoch;
  }
  auto worker = kernel::GuestThread::CreateHost([epoch] { return WorkerMain(epoch); }, kWorkerStackSize,
                                                "Audio Worker");
  std::lock_guard lock(s.mutex);
  if (!worker) {
    s.worker_exited = true;
    if (!s.worker_failed_logged) LogLine("[audio] cannot create the Audio Worker thread");
    s.worker_failed_logged = true;
    return;
  }
  s.worker = std::move(worker);
}

bool ValidHandle(uint32_t handle) {
  return (handle & 0xFFFF0000) == kDriverTag && (handle & 0xFFFF) < kMaxClients;
}

}  // namespace

void ConvertFrame(const uint8_t* guest_be_planar, std::span<float, plat::kAudioFrameFloats> out) {
  constexpr uint32_t kChannels = plat::kAudioChannels, kSamples = plat::kAudioFrameSamples;
  for (uint32_t c = 0; c < kChannels; ++c) {
    const uint8_t* channel = guest_be_planar + size_t(c) * kSamples * 4;
    for (uint32_t i = 0; i < kSamples; ++i) {
      uint32_t bits;
      std::memcpy(&bits, channel + size_t(i) * 4, 4);
      out[size_t(i) * kChannels + c] = std::bit_cast<float>(std::byteswap(bits));
    }
  }
}

std::optional<plat::AudioSinkKind> ParseAudioSink(std::string_view name) {
  if (name == "xaudio2") return plat::AudioSinkKind::kXAudio2;
  if (name == "null") return plat::AudioSinkKind::kNull;
  return std::nullopt;
}

bool Init(const AudioOptions& options, SinkFactory factory) {
  State& s = S();
  std::lock_guard life(g_lifecycle);
  {
    std::lock_guard lock(s.mutex);
    if (s.initialized) return true;
  }
  std::unique_ptr<WavWriter> capture;
  if (options.capture) {
    capture = WavWriter::Create(*options.capture);
    if (!capture) return false;
  }
  AudioOptions effective = options;
  if (!factory) factory = &PlatformFactory;
  {
    std::lock_guard lock(g_capture_mutex);
    g_capture = std::move(capture);
  }
  std::lock_guard lock(s.mutex);
  s.options = effective;
  s.factory = std::move(factory);
  s.initialized = true;
  s.running = true;
  ++s.epoch;
  s.worker_exited = true;
  s.in_callback = -1;
  s.next = 0;
  return true;
}

void Shutdown() {
  State& s = S();
  std::lock_guard life(g_lifecycle);
  std::shared_ptr<kernel::GuestThread> worker;
  {
    std::lock_guard lock(s.mutex);
    s.initialized = false;
    s.running = false;
    worker = std::move(s.worker);
    Wake(s);
    s.idle_cv.notify_all();
  }
  // The worker finishes the running callback and exits; if the game callback
  // is blocked (or Shutdown comes from inside the callback, KeBugCheck) it is
  // not waited for beyond kShutdownWaitMs: the orderly exit proceeds.
  bool worker_done = true;
  if (worker) {
    std::unique_lock lock(s.mutex);
    worker_done = !t_is_worker && s.idle_cv.wait_for(lock, std::chrono::milliseconds(kShutdownWaitMs),
                                                     [&] { return s.worker_exited; });
    if (!worker_done) LogLine("[audio] Shutdown: Audio Worker still inside a callback, not waited for");
  }
  worker.reset();  // if the thread is still alive it keeps itself alive (GuestThread::self_)

  std::array<std::unique_ptr<plat::AudioOut>, kMaxClients> sinks;
  std::vector<uint32_t> frees;
  {
    std::lock_guard lock(s.mutex);
    for (uint32_t i = 0; i < kMaxClients; ++i) {
      Client& c = s.clients[i];
      if (!c.in_use) continue;
      sinks[i] = std::move(c.sink);
      if (worker_done || s.in_callback != int(i)) frees.push_back(c.wrapped_arg);
      c = Client{};
      s.returned[i].store(0);
    }
    if (worker_done) {
      frees.insert(frees.end(), s.deferred_free.begin(), s.deferred_free.end());
      s.deferred_free.clear();
    }
    s.factory = {};
  }
  // Outside the lock: destroying a sink waits for its threads (XAudio2 engine,
  // null pacing) and for the running on_done calls; BufferDone takes no lock
  // (atomics).
  for (auto& sink : sinks) sink.reset();
  for (uint32_t a : frees) FreeGuest(a);
  std::lock_guard lock(g_capture_mutex);
  if (g_capture) g_capture->Close();
  g_capture.reset();
}

// Xenia XAudioRegisterRenderDriverClient_entry + AudioSystem::RegisterClient.
uint32_t RegisterClient(uint32_t callback_ptr, uint32_t driver_handle_out) {
  State& s = S();
  const be<uint32_t>* cb = kernel::GuestPtr<be<uint32_t>>(callback_ptr);
  if (!cb) return kStatusUnsuccessful;
  const uint32_t callback = cb[0], arg = cb[1];
  kernel::MemorySystem* m = kernel::Memory();
  const uint32_t wrapped = m ? m->heap.Alloc(4) : 0;
  if (!wrapped) return kStatusUnsuccessful;
  *kernel::GuestPtr<be<uint32_t>>(wrapped) = arg;

  uint32_t index = kMaxClients;
  uint64_t generation = 0;
  plat::AudioSinkKind kind = plat::AudioSinkKind::kNull;
  bool mute = false;
  SinkFactory factory;
  {
    std::lock_guard lock(s.mutex);
    if (s.initialized)
      for (uint32_t i = 0; i < kMaxClients; ++i)
        if (!s.clients[i].in_use) {
          index = i;
          break;
        }
    if (index < kMaxClients) {
      Client& c = s.clients[index];
      c = Client{};
      c.in_use = true;
      c.generation = generation = ++s.generations;
      s.returned[index].store(ReturnedWord(generation));
      kind = s.options.sink;
      mute = s.options.mute;
      factory = s.factory;
    }
  }
  if (index == kMaxClients) {
    FreeGuest(wrapped);          // outside the lock (process heap)
    return kStatusUnsuccessful;  // 8 clients in use (or system stopped)
  }
  // Outside the lock: creating the sink starts its thread, writing the
  // handle touches guest memory (pages watched by physical_watch).
  auto sink = factory(kind, [index, generation] { BufferDone(index, generation); });
  if (sink) sink->SetMuted(mute);
  if (be<uint32_t>* out = kernel::GuestPtr<be<uint32_t>>(driver_handle_out)) *out = kDriverTag | index;
  {
    std::lock_guard lock(s.mutex);
    Client& c = s.clients[index];
    if (sink && s.initialized && c.in_use && c.generation == generation) {
      c.sink = std::move(sink);
      c.callback = callback;
      c.wrapped_arg = wrapped;
      c.credits = kClientCredits;
      c.runnable = true;
      Wake(s);
    } else {
      if (c.in_use && c.generation == generation) {
        c = Client{};
        s.returned[index].store(0);
      }
      generation = 0;
    }
  }
  if (!generation) {  // sink not created or Shutdown in the meantime
    sink.reset();
    FreeGuest(wrapped);
    return kStatusUnsuccessful;
  }
  EnsureWorker();
  return 0;
}

// Xenia XAudioUnregisterRenderDriverClient_entry + AudioSystem::UnregisterClient,
// plus waiting for the running callback (Review Focus 1).
uint32_t UnregisterClient(uint32_t handle) {
  if (!ValidHandle(handle)) return 0;
  const uint32_t index = handle & 0xFFFF;
  State& s = S();
  std::unique_lock lock(s.mutex);
  Client& c = s.clients[index];
  if (!c.in_use) return 0;
  const uint64_t generation = c.generation;
  c.runnable = false;
  c.credits = 0;
  const bool from_own_callback = t_is_worker && s.in_callback == int(index);
  if (!from_own_callback) {
    // Unbounded wait (after returning the callback must no longer run); a
    // line after ~2 s makes a stall diagnosable.
    const auto done = [&] { return s.in_callback != int(index) || !s.running; };
    if (!s.idle_cv.wait_for(lock, std::chrono::milliseconds(kUnregisterLogMs), done)) {
      char line[96];
      std::snprintf(line, sizeof(line), "[audio] Unregister waiting for the callback (client %u)", index);
      LogLine(line);
      s.idle_cv.wait(lock, done);
    }
  }
  if (!c.in_use || c.generation != generation) return 0;  // unregistered by another call
  std::unique_ptr<plat::AudioOut> sink = std::move(c.sink);
  uint32_t arg = c.wrapped_arg;
  if (s.in_callback == int(index)) {  // from the callback itself (or blocked worker): free later
    s.deferred_free.push_back(arg);
    arg = 0;
  }
  c = Client{};
  s.returned[index].store(0);
  lock.unlock();
  // Outside the lock: stops the sink threads and waits for the running on_done
  // calls (BufferDone takes no lock: only atomics and Wake).
  sink.reset();
  FreeGuest(arg);
  return 0;
}

// Xenia XAudioSubmitRenderDriverFrame_entry + AudioSystem::SubmitFrame.
uint32_t SubmitFrame(uint32_t handle, uint32_t samples_ptr) {
  if (!ValidHandle(handle) || !samples_ptr ||
      !kernel::GuestRangeValid(samples_ptr, plat::kAudioFrameFloats * sizeof(float)))
    return 0;
  std::array<float, plat::kAudioFrameFloats> frame;
  ConvertFrame(kernel::GuestPtr<uint8_t>(samples_ptr), frame);
  State& s = S();
  {
    std::lock_guard lock(s.mutex);
    Client& c = s.clients[handle & 0xFFFF];
    if (!c.in_use || !c.sink) return 0;
    c.sink->Submit(frame);  // queue full: frame dropped
  }
  {
    std::lock_guard lock(g_capture_mutex);
    if (g_capture) g_capture->Append(frame);
  }
  kernel::stats::NoteAudioFrameSubmitted();
  return 0;
}

uint64_t CaptureSampleFramesWritten() {
  std::lock_guard lock(g_capture_mutex);
  return g_capture ? g_capture->SampleFramesWritten() : 0;
}

}  // namespace apu
