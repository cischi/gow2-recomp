// Fake GPU: backend with the command processor thread (see gpu.h).
//
// Semantica da Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee,
// BSD): src/xenia/gpu/command_processor.cc (WorkerThreadMain,
// InitializeRingBuffer, EnableReadPointerWriteBack, UpdateWritePointer,
// WriteRegister, increment_counter).
//
// Deviations: the registers are plain guest memory (no MMIO): the write
// pointer is read from CP_RB_WPTR on every thread loop instead of being
// notified by the write; the read pointer is also written to
// CP_RB_RPTR (like the hardware, Xenia does not).
#include "gpu_fake/gpu.h"

// First: kernel/function.h before the headers that would bring in
// ppc_config.h (see kernel/function.h).
#include "kernel/function.h"

#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "gpu/xenos.h"
#include "gpu_fake/pm4.h"
#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/sync.h"
#include "kernel/threads.h"
#include "log.h"
#include "platform/platform.h"

namespace gpu_fake {
namespace {

class FakeBackend final : public gpu::Backend {
 public:
  FakeBackend() = default;
  ~FakeBackend() override { Stop(); }

  bool Start() override {
    std::lock_guard g(mutex_);
    if (cp_thread_) return true;
    running_ = true;
    cp_thread_ = kernel::GuestThread::CreateHost([this] { return CommandProcessorMain(); }, 0,
                                                 "GPU command processor");
    if (!cp_thread_) {
      running_ = false;
      LogLine("[gpu] cannot create the fake GPU command processor thread");
      return false;
    }
    LogLine("[gpu] fake GPU: registers 0x7FC80000-0x7FCFFFFF, command processor and vblank at 60 Hz");
    return true;
  }

  void Stop() override {
    std::shared_ptr<kernel::GuestThread> cp;
    {
      std::lock_guard g(mutex_);
      running_ = false;
      cp = std::move(cp_thread_);
    }
    // Outside the lock: the command processor takes it on every loop.
    if (!cp) return;
    const std::shared_ptr<kernel::KernelObject> one[1] = {cp};
    kernel::WaitAny(one, nullptr, false);
    cp.reset();  // the thread has finished: the destructor joins
  }

  // Xenia CommandProcessor::InitializeRingBuffer.
  void InitializeRingBuffer(uint32_t phys, uint32_t size_log2) override {
    std::lock_guard g(mutex_);
    ring_phys_ = phys;
    ring_dwords_ = size_log2 < 29 ? (1u << (size_log2 + 3)) / 4 : 0;
    ++ring_generation_;
  }

  // Xenia CommandProcessor::EnableReadPointerWriteBack (the update frequency
  // is not needed: the read pointer is written after every block).
  void EnableReadPointerWriteBack(uint32_t phys, uint32_t /*block_size_log2*/) override { writeback_phys_ = phys; }

  void OnVblank() override { counter_.fetch_add(1); }

  gpu::Counters GetCounters() const override { return {packets_.load(), unknown_packets_.load(), frames_.load()}; }

 private:
  // Xenia CommandProcessor::WriteRegister: register write from the command
  // processor (PM4 packets), with side effects: SCRATCH_REG0..7 with the bit
  // enabled in SCRATCH_UMSK are also written (big-endian) to physical memory
  // at SCRATCH_ADDR + i*4 (fence towards RAM; REG0 = "interrupt sync");
  // COHER_STATUS_HOST receives bit 31 (status: to be made coherent at the
  // next WAIT_REG_MEM). CPU writes (memory) do not go through here.
  static void CpWriteRegister(uint32_t reg, uint32_t value) {
    using namespace gpu;
    if (reg >= kRegisterCount) return;
    if (reg >= kRegScratchReg0 && reg <= kRegScratchReg0 + 7) {
      WriteRegister(reg, value);
      const uint32_t i = reg - kRegScratchReg0;
      if ((1u << i) & ReadRegister(kRegScratchUmsk)) {
        const uint32_t addr = ReadRegister(kRegScratchAddr) + i * 4;
        *kernel::GuestPtr<be<uint32_t>>(PhysicalToGuest(addr)) = value;
      }
    } else if (reg == kRegCoherStatusHost) {
      WriteRegister(reg, value | 0x80000000u);
    } else {
      WriteRegister(reg, value);
    }
  }

  // Xenia CommandProcessor::WorkerThreadMain: waits for a write pointer
  // different from the read pointer, executes the packets, writes the read
  // pointer.
  uint32_t CommandProcessorMain() {
    using namespace gpu;
    Pm4State pm4;
    Pm4Effects fx;
    fx.write_reg = [](uint32_t reg, uint32_t value) { CpWriteRegister(reg, value); };
    fx.store_reg = [](uint32_t reg, uint32_t value) { WriteRegister(reg, value); };
    fx.read_reg = [](uint32_t reg) { return ReadRegister(reg); };
    fx.interrupt = [](uint32_t cpu) { DispatchInterrupt(1, cpu); };
    fx.counter = [this] { return counter_.load(); };
    fx.swap = [this](uint32_t, uint32_t, uint32_t) {
      counter_.fetch_add(1);  // Xenia XE_SWAP: ++counter_
    };
    fx.running = [this] { return running_.load(); };
    fx.state = &pm4;

    uint32_t generation = ~0u, read = 0, idle = 0;
    while (running_.load()) {
      uint32_t ring_phys, ring_dwords;
      {
        std::lock_guard g(mutex_);
        ring_phys = ring_phys_;
        ring_dwords = ring_dwords_;
        if (generation != ring_generation_) {
          generation = ring_generation_;
          read = 0;
        }
      }
      const uint32_t write = ring_dwords ? ReadRegister(kRegCpRbWptr) % ring_dwords : read;
      if (!ring_dwords || write == read) {
        // Xenia: spins for a while, then waits in short intervals.
        if (++idle < 2000)
          std::this_thread::yield();
        else
          plat::SleepPrecise(250);
        continue;
      }
      idle = 0;
      read = ExecuteRing(kernel::g_base, PhysicalToGuest(ring_phys), ring_dwords, read, write, fx);
      packets_.store(pm4.counters.packets, std::memory_order_relaxed);
      unknown_packets_.store(pm4.counters.unknown_packets, std::memory_order_relaxed);
      frames_.store(pm4.counters.frames, std::memory_order_relaxed);
      WriteRegister(kRegCpRbRptr, read);
      if (const uint32_t wb = writeback_phys_.load())
        std::atomic_ref<uint32_t>(*kernel::GuestPtr<uint32_t>(PhysicalToGuest(wb)))
            .store(std::byteswap(read), std::memory_order_release);
    }
    return 0;
  }

  std::mutex mutex_;  // thread and ring configuration
  std::atomic<bool> running_{false};
  std::shared_ptr<kernel::GuestThread> cp_thread_;

  // Ring (under mutex_): physical address, DWORDs, generation (on every
  // VdInitializeRingBuffer the command processor restarts from read 0).
  uint32_t ring_phys_ = 0;
  uint32_t ring_dwords_ = 0;
  uint32_t ring_generation_ = 0;
  std::atomic<uint32_t> writeback_phys_{0};

  std::atomic<uint32_t> counter_{0};  // Xenia CommandProcessor::counter_

  // Copy of the counters published by the command processor.
  std::atomic<uint64_t> packets_{0}, unknown_packets_{0}, frames_{0};
};

}  // namespace

std::unique_ptr<gpu::Backend> CreateBackend() { return std::make_unique<FakeBackend>(); }

}  // namespace gpu_fake
