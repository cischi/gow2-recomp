// Xenia CommandProcessor without a GPU for the tests (tests/gpu/xenia_cp_test.cpp):
// the ring loop, the PM4 packets and the registers are the vendored ones
// (thirdparty/xenia/src/xenia/gpu/command_processor.cc); the parts that in
// Xenia the graphics backend provides are fake:
//  - SetupContext true, no shader (LoadShader nullptr);
//  - IssueDraw returns draw_result (configurable) and counts the calls;
//  - IssueCopy true;
//  - IssueSwap records the arguments.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"

namespace gow2::test {

class NullCommandProcessor final : public xe::gpu::CommandProcessor {
 public:
  explicit NullCommandProcessor(xe::gpu::GraphicsSystem* graphics_system)
      : CommandProcessor(graphics_system, graphics_system->kernel_state()) {}

  // (frontbuffer, width, height) of each IssueSwap.
  std::vector<std::array<uint32_t, 3>> Swaps() {
    std::lock_guard lock(mutex_);
    return swaps_;
  }

  std::atomic<bool> draw_result{true};
  std::atomic<int> draws{0};

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    std::lock_guard lock(mutex_);
    swaps_.push_back({frontbuffer_ptr, frontbuffer_width, frontbuffer_height});
  }
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override {}
  void RestoreEdramSnapshot(const void*) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  xe::gpu::Shader* LoadShader(xe::gpu::xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(xe::gpu::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override {
    ++draws;
    return draw_result.load();
  }
  bool IssueCopy() override { return true; }

 private:
  std::mutex mutex_;
  std::vector<std::array<uint32_t, 3>> swaps_;
};

}  // namespace gow2::test
