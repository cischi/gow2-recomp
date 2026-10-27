// Shim of Xenia's src/xenia/gpu/trace_writer.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): same interface, no trace file. is_open() is
// always false and all writes are empty. trace_protocol.h (vendored
// from canary, used by packet_disassembler.h) provides EventCommand.
#ifndef XENIA_GPU_TRACE_WRITER_H_
#define XENIA_GPU_TRACE_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "xenia/gpu/registers.h"
#include "xenia/gpu/trace_protocol.h"

namespace xe {
namespace gpu {

class TraceWriter {
 public:
  explicit TraceWriter(uint8_t* /*membase*/) {}

  bool is_open() const { return false; }
  bool Open(const std::filesystem::path& /*path*/, uint32_t /*title_id*/) { return false; }
  void Flush() {}
  void Close() {}

  void WritePrimaryBufferStart(uint32_t /*base_ptr*/, uint32_t /*count*/) {}
  void WritePrimaryBufferEnd() {}
  void WriteIndirectBufferStart(uint32_t /*base_ptr*/, uint32_t /*count*/) {}
  void WriteIndirectBufferEnd() {}
  void WritePacketStart(uint32_t /*base_ptr*/, uint32_t /*count*/) {}
  void WritePacketEnd() {}
  void WriteMemoryRead(uint32_t /*base_ptr*/, size_t /*length*/, const void* /*host_ptr*/ = nullptr) {}
  void WriteMemoryReadCached(uint32_t /*base_ptr*/, size_t /*length*/) {}
  void WriteMemoryReadCachedNop(uint32_t /*base_ptr*/, size_t /*length*/) {}
  void WriteMemoryWrite(uint32_t /*base_ptr*/, size_t /*length*/, const void* /*host_ptr*/ = nullptr) {}
  void WriteEdramSnapshot(const void* /*snapshot*/) {}
  void WriteEvent(EventCommand::Type /*event_type*/) {}
  void WriteRegisters(uint32_t /*first_register*/, const uint32_t* /*register_values*/,
                      uint32_t /*register_count*/, bool /*execute_callbacks_on_play*/) {}
  void WriteGammaRamp(const reg::DC_LUT_30_COLOR* /*gamma_ramp_256_entry_table*/,
                      const reg::DC_LUT_PWL_DATA* /*gamma_ramp_pwl_rgb*/,
                      uint32_t /*gamma_ramp_rw_component*/) {}
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_TRACE_WRITER_H_
