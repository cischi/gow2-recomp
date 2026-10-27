// Audio outputs: null sink (consumes buffers at the real rate of 187.5/s) and
// XAudio2 2.9 with an internal fallback to null pacing after a critical error.
// XAudio2 model: src/xenia/apu/xaudio2/xaudio2_audio_driver.cc of Xenia
// @ 95a5c3ee (BSD): DLL loaded at runtime, MTA thread for the lifetime
// of the COM objects, 5.1 float source voice at 48 kHz.
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"
#include "platform.h"

namespace plat {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kPeriod = std::chrono::nanoseconds(1'000'000'000LL * kAudioFrameSamples / kAudioSampleRate);
constexpr auto kMaxLag = std::chrono::milliseconds(100);

// Takes a buffer from the queue (if there is one). Every buffer accepted by
// Submit is consumed exactly once, even if two consumers compete.
bool TakeQueued(std::atomic<uint32_t>& queued) {
  uint32_t n = queued.load();
  do {
    if (n == 0) return false;
  } while (!queued.compare_exchange_weak(n, n - 1));
  return true;
}

bool ReserveQueued(std::atomic<uint32_t>& queued) {
  uint32_t n = queued.load();
  do {
    if (n >= kAudioMaxQueued) return false;
  } while (!queued.compare_exchange_weak(n, n + 1));
  return true;
}

// Null pacing: one buffer every 5333 us with absolute deadlines (SleepPrecise),
// until stop becomes true. Empty queue: polling every 0.5 ms without
// catching up on the elapsed time; delay limited to kMaxLag (no bursts).
void PaceNull(std::atomic<uint32_t>& queued, const std::atomic<bool>& stop, const AudioOut::BufferDone& on_done) {
  auto next = Clock::now() + kPeriod;
  while (!stop.load()) {
    if (queued.load() == 0) {
      SleepPrecise(500);
      next = Clock::now() + kPeriod;
      continue;
    }
    const auto now = Clock::now();
    if (now < next) {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(next - now).count();
      SleepPrecise(uint32_t(us > 0 ? us : 1));
      continue;  // re-evaluate stop and the queue
    }
    if (TakeQueued(queued) && on_done) on_done();
    next += kPeriod;
    if (Clock::now() - next > kMaxLag) next = Clock::now();
  }
}

class NullAudioOut final : public AudioOut {
 public:
  explicit NullAudioOut(BufferDone on_done) : on_done_(std::move(on_done)) {
    thread_ = std::thread([this] {
      SetCurrentThreadName("audio-null");
      PaceNull(queued_, stop_, on_done_);
    });
  }
  ~NullAudioOut() override {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  bool Submit(std::span<const float, kAudioFrameFloats>) override { return ReserveQueued(queued_); }
  void SetMuted(bool) override {}
  AudioSinkKind Kind() const override { return AudioSinkKind::kNull; }

 private:
  BufferDone on_done_;
  std::atomic<uint32_t> queued_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// ---- XAudio2 2.9 -----------------------------------------------------------

using XAudio2CreateFn = HRESULT(__stdcall*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (ksmedia.h) without INITGUID.
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr DWORD kChannelMask51 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                                 SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
static_assert(kChannelMask51 == 0x3F);

HRESULT LastErrorHr() {
  const DWORD e = GetLastError();
  return e ? HRESULT_FROM_WIN32(e) : E_FAIL;
}

// Concurrency. on_done runs on the XAudio2 engine thread (OnBufferEnd) or on the
// null pacing thread (after a critical error) and may call Submit again.
// No lock of this class is held during a call to XAudio2 or to
// on_done: slots_mutex_ only protects the list of free buffers.
// Accounting: queued_ counts the buffers accepted and not yet consumed; each
// unit is taken exactly once (TakeQueued) by OnBufferEnd (XAudio2 mode) or
// by the null pacing (after the critical error), so on_done is called at most
// once per accepted buffer, even during the mode switch.
class XAudio2AudioOut final : public AudioOut {
 public:
  explicit XAudio2AudioOut(BufferDone on_done) : on_done_(std::move(on_done)), voice_cb_(this), engine_cb_(this) {
    free_slots_.reserve(kAudioMaxQueued);
    for (uint32_t i = kAudioMaxQueued; i-- > 0;) free_slots_.push_back(i);
  }

  ~XAudio2AudioOut() override {
    stopping_.store(true);
    // The null pacing stops first; then the OnBufferEnd calls in progress are
    // awaited (later ones see stopping_ and do not call on_done).
    {
      std::lock_guard lock(pacer_mutex_);
      pacer_wake_ = true;
    }
    pacer_cv_.notify_all();
    if (pacer_.joinable()) pacer_.join();
    while (callbacks_in_flight_.load() != 0) std::this_thread::yield();
    // The MTA thread destroys voices and engine (DestroyVoice waits for the engine).
    {
      std::lock_guard lock(life_mutex_);
      shutdown_requested_ = true;
    }
    life_cv_.notify_all();
    if (mta_.joinable()) mta_.join();
    if (module_) FreeLibrary(module_);
  }

  // Loads the DLL and creates the objects on the MTA thread. S_OK or the error.
  HRESULT Start() {
    module_ = LoadLibraryW(L"xaudio2_9.dll");
    if (!module_) return LastErrorHr();
    create_ = reinterpret_cast<XAudio2CreateFn>(GetProcAddress(module_, "XAudio2Create"));
    if (!create_) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    pacer_ = std::thread([this] { PacerMain(); });
    mta_ = std::thread([this] { MtaMain(); });
    std::unique_lock lock(life_mutex_);
    life_cv_.wait(lock, [this] { return init_done_; });
    return init_hr_;
  }

  bool Submit(std::span<const float, kAudioFrameFloats> interleaved) override {
    if (stopping_.load() || !ReserveQueued(queued_)) return false;
    if (fallback_.load()) return true;  // null pacing: the content is not played
    uint32_t slot;
    {
      std::lock_guard lock(slots_mutex_);
      if (free_slots_.empty()) {  // only during the switch to null pacing
        slot = kAudioMaxQueued;
      } else {
        slot = free_slots_.back();
        free_slots_.pop_back();
      }
    }
    if (slot == kAudioMaxQueued) return fallback_.load() ? true : (TakeQueued(queued_), false);
    std::memcpy(buffers_[slot].data(), interleaved.data(), sizeof(float) * kAudioFrameFloats);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = UINT32(sizeof(float) * kAudioFrameFloats);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(buffers_[slot].data());
    buffer.pContext = reinterpret_cast<void*>(uintptr_t(slot));
    const HRESULT hr = fail_next_submit_.exchange(false) ? XAUDIO2_E_DEVICE_INVALIDATED  // tests only
                                                         : voice_->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
      // The buffer is already counted in queued_: the null pacing consumes it like
      // the others, so the client's credit is not lost.
      ReleaseSlot(slot);
      CriticalError(hr);
    }
    return true;
  }

  // Tests only (SimulateAudioSubmitFailure): the next SubmitSourceBuffer
  // fails.
  void FailNextSubmit() { fail_next_submit_.store(true); }

  void SetMuted(bool muted) override {
    if (master_) master_->SetVolume(muted ? 0.0f : 1.0f);
  }
  AudioSinkKind Kind() const override { return AudioSinkKind::kXAudio2; }

  // IXAudio2EngineCallback::OnCriticalError (also simulated by the tests).
  void CriticalError(HRESULT hr) {
    if (fallback_.exchange(true)) return;
    char line[96];
    std::snprintf(line, sizeof(line), "[audio] xaudio2 critical error (hr=0x%08X), using the null sink", uint32_t(hr));
    LogLine(line);
    {
      std::lock_guard lock(pacer_mutex_);
      pacer_wake_ = true;
    }
    pacer_cv_.notify_all();
  }

 private:
  struct VoiceCallback final : IXAudio2VoiceCallback {
    explicit VoiceCallback(XAudio2AudioOut* o) : owner(o) {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override { owner->BufferEnd(uint32_t(uintptr_t(context))); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
    XAudio2AudioOut* owner;
  };
  struct EngineCallback final : IXAudio2EngineCallback {
    explicit EngineCallback(XAudio2AudioOut* o) : owner(o) {}
    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) noexcept override { owner->CriticalError(hr); }
    XAudio2AudioOut* owner;
  };

  void ReleaseSlot(uint32_t slot) {
    if (slot >= kAudioMaxQueued) return;
    std::lock_guard lock(slots_mutex_);
    free_slots_.push_back(slot);
  }

  // XAudio2 engine thread. After the critical error the buffers the engine
  // still finishes do not count: the null pacing consumes them.
  void BufferEnd(uint32_t slot) {
    callbacks_in_flight_.fetch_add(1);
    ReleaseSlot(slot);
    if (!stopping_.load() && !fallback_.load() && TakeQueued(queued_) && on_done_) on_done_();
    callbacks_in_flight_.fetch_sub(1);
  }

  // Idle until a critical error arrives, then null pacing.
  void PacerMain() {
    SetCurrentThreadName("audio-xaudio2-null");
    {
      std::unique_lock lock(pacer_mutex_);
      pacer_cv_.wait(lock, [this] { return pacer_wake_; });
    }
    if (!stopping_.load()) PaceNull(queued_, stopping_, on_done_);
  }

  void MtaMain() {
    SetCurrentThreadName("audio-xaudio2-mta");
    // Like Xenia: the MTA thread keeps the implicit apartment alive for the (guest)
    // threads that call Submit without having initialized COM.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com = SUCCEEDED(hr);
    if (com) hr = CreateObjects();
    {
      std::lock_guard lock(life_mutex_);
      init_hr_ = SUCCEEDED(hr) ? S_OK : hr;
      init_done_ = true;
    }
    life_cv_.notify_all();
    if (SUCCEEDED(hr)) {
      std::unique_lock lock(life_mutex_);
      life_cv_.wait(lock, [this] { return shutdown_requested_; });
    }
    DestroyObjects();
    if (com) CoUninitialize();
  }

  HRESULT CreateObjects() {
    HRESULT hr = create_(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) return hr;
    if (!xaudio_) return E_POINTER;
    hr = xaudio_->RegisterForCallbacks(&engine_cb_);
    if (FAILED(hr)) return hr;
    hr = xaudio_->CreateMasteringVoice(&master_);
    if (FAILED(hr)) return hr;
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = WORD(kAudioChannels);
    format.Format.nSamplesPerSec = kAudioSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = WORD(kAudioChannels * 4);
    format.Format.nAvgBytesPerSec = kAudioSampleRate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = kChannelMask51;
    format.SubFormat = kSubtypeIeeeFloat;
    hr = xaudio_->CreateSourceVoice(&voice_, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &voice_cb_);
    if (FAILED(hr)) return hr;
    return voice_->Start(0);
  }

  void DestroyObjects() {
    if (voice_) {
      voice_->Stop(0);
      voice_->DestroyVoice();  // waits for the voice callbacks to finish
      voice_ = nullptr;
    }
    if (master_) {
      master_->DestroyVoice();
      master_ = nullptr;
    }
    if (xaudio_) {
      xaudio_->UnregisterForCallbacks(&engine_cb_);
      xaudio_->StopEngine();
      xaudio_->Release();
      xaudio_ = nullptr;
    }
  }

  BufferDone on_done_;
  VoiceCallback voice_cb_;
  EngineCallback engine_cb_;

  HMODULE module_ = nullptr;
  XAudio2CreateFn create_ = nullptr;
  IXAudio2* xaudio_ = nullptr;
  IXAudio2MasteringVoice* master_ = nullptr;
  IXAudio2SourceVoice* voice_ = nullptr;

  std::mutex life_mutex_;
  std::condition_variable life_cv_;
  bool init_done_ = false;
  HRESULT init_hr_ = E_FAIL;
  bool shutdown_requested_ = false;
  std::thread mta_;

  std::mutex pacer_mutex_;
  std::condition_variable pacer_cv_;
  bool pacer_wake_ = false;
  std::thread pacer_;

  std::mutex slots_mutex_;  // leaf: never held during XAudio2 or on_done
  std::vector<uint32_t> free_slots_;
  std::array<std::array<float, kAudioFrameFloats>, kAudioMaxQueued> buffers_{};

  std::atomic<uint32_t> queued_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> fallback_{false};
  std::atomic<bool> fail_next_submit_{false};
  std::atomic<uint32_t> callbacks_in_flight_{0};
};

}  // namespace

std::unique_ptr<AudioOut> CreateNullAudioOut(AudioOut::BufferDone on_done) {
  return std::make_unique<NullAudioOut>(std::move(on_done));
}

std::unique_ptr<AudioOut> CreateXAudio2AudioOut(AudioOut::BufferDone on_done, uint32_t* hr_out) {
  HRESULT hr = E_FAIL;
  std::unique_ptr<XAudio2AudioOut> out;
  try {
    out = std::make_unique<XAudio2AudioOut>(std::move(on_done));
    hr = out->Start();
  } catch (...) {  // std::thread or allocation: fall back as for an XAudio2 error
    hr = E_FAIL;
  }
  if (FAILED(hr)) {
    out.reset();  // stops the threads and destroys what was created
    if (hr_out) *hr_out = uint32_t(hr) ? uint32_t(hr) : uint32_t(E_FAIL);
    return nullptr;
  }
  if (hr_out) *hr_out = 0;
  return out;
}

void SimulateAudioCriticalError(AudioOut& out) {
  if (auto* x = dynamic_cast<XAudio2AudioOut*>(&out)) x->CriticalError(XAUDIO2_E_DEVICE_INVALIDATED);
}

void SimulateAudioSubmitFailure(AudioOut& out) {
  if (auto* x = dynamic_cast<XAudio2AudioOut*>(&out)) x->FailNextSubmit();
}

}  // namespace plat
