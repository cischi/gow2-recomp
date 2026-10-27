// GPU backend around Xenia's CommandProcessor (see xenia_backend.h) and the
// gow2::hooks called by the patches to the vendored code.
//
// Semantics from Xenia (https://github.com/xenia-project/xenia @ 95a5c3ee,
// BSD): src/xenia/gpu/graphics_system.cc (Setup: CreateCommandProcessor,
// Initialize; Shutdown; InitializeRingBuffer; EnableReadPointerWriteBack;
// MarkVblank: increment_counter).
#include "gpu/xenia_backend.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

#include "gow2_hooks.h"
#include "kernel/stats.h"
#include "log.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"

namespace {
// Type 3 packets not executed (per process) and opcodes already logged.
std::atomic<uint64_t> g_unknown_packets{0};
std::array<std::atomic<bool>, 128> g_unknown_logged{};
}  // namespace

namespace gow2::hooks {

void OnDraw(bool succeeded) { kernel::stats::NoteGpuDraw(succeeded); }

void OnEdramTransfers(uint32_t count) { kernel::stats::NoteGpuEdramTransfers(count); }

void OnUnknownPacket(uint32_t opcode) {
  g_unknown_packets.fetch_add(1, std::memory_order_relaxed);
  kernel::stats::NoteGpuUnknownPacket(opcode);
  if (opcode < g_unknown_logged.size() && !g_unknown_logged[opcode].exchange(true)) {
    // As in Xenia, ExecutePacketType3 returns false: the CP discards the rest of the
    // primary or indirect buffer in progress (gpu_fake skipped only the packet).
    char line[96];
    std::snprintf(line, sizeof(line), "[gpu] PM4 packet not executed: opcode 0x%02X (rest of buffer discarded)",
                  opcode);
    LogLine(line);
  }
}

}  // namespace gow2::hooks

namespace gpu {
namespace {

class XeniaBackend final : public Backend {
 public:
  explicit XeniaBackend(std::unique_ptr<xe::gpu::GraphicsSystem> graphics_system)
      : graphics_system_(std::move(graphics_system)), cp_(graphics_system_->command_processor()) {}

  ~XeniaBackend() override { Stop(); }

  bool Start() override {
    std::lock_guard g(mutex_);
    if (started_) return true;
    if (!cp_->Initialize()) {
      LogLine("[gpu] Xenia command processor initialization failed");
      return false;
    }
    started_ = true;
    LogLine("[gpu] Xenia command processor started (GPU Commands thread)");
    return true;
  }

  // Xenia CommandProcessor::Shutdown: worker_running_ = false and wait for the
  // thread; a WAIT_REG_MEM in progress exits within ~1 ms (gow2-wait-reg-mem patch).
  void Stop() override {
    std::lock_guard g(mutex_);
    if (!started_) return;
    started_ = false;
    cp_->Shutdown();
  }

  void InitializeRingBuffer(uint32_t phys, uint32_t size_log2) override {
    graphics_system_->InitializeRingBuffer(phys, size_log2);
  }
  void EnableReadPointerWriteBack(uint32_t phys, uint32_t block_size_log2) override {
    graphics_system_->EnableReadPointerWriteBack(phys, block_size_log2);
  }

  // Xenia MarkVblank: the CP counter (written by EVENT_WRITE_SHD with bit
  // 31) advances on every vblank. As in Xenia without synchronization with the
  // CP thread (a uint32_t).
  void OnVblank() override { cp_->increment_counter(); }

  // Xenia's CommandProcessor counts neither packets nor frames: only the
  // packets not executed (gow2 hooks, per process).
  Counters GetCounters() const override { return {0, g_unknown_packets.load(std::memory_order_relaxed), 0}; }

 private:
  std::mutex mutex_;  // Start/Stop
  bool started_ = false;
  std::unique_ptr<xe::gpu::GraphicsSystem> graphics_system_;
  xe::gpu::CommandProcessor* cp_;
};

}  // namespace

std::unique_ptr<Backend> CreateXeniaBackend(std::unique_ptr<xe::gpu::GraphicsSystem> graphics_system,
                                            CommandProcessorFactory factory) {
  if (!graphics_system || !factory) return nullptr;
  std::unique_ptr<xe::gpu::CommandProcessor> cp = factory(graphics_system.get());
  if (!cp) return nullptr;
  graphics_system->gow2_SetCommandProcessor(std::move(cp));
  return std::make_unique<XeniaBackend>(std::move(graphics_system));
}

}  // namespace gpu
