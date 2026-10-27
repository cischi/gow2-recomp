// 5.1 32-bit float WAV writer (WAVE_FORMAT_EXTENSIBLE, 48 kHz).
// The RIFF and data sizes are rewritten every 48000 samples per channel and
// on Close: a _Exit leaves a file that is valid up to the last second.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>

namespace apu {

class WavWriter {
 public:
  // nullptr if the file cannot be opened.
  static std::unique_ptr<WavWriter> Create(const std::filesystem::path&);
  ~WavWriter();
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  // Multiple of 6 samples (any remainder is ignored).
  void Append(std::span<const float> interleaved_6ch);
  // Idempotent; also called by the destructor.
  void Close();
  // Samples per channel.
  uint64_t SampleFramesWritten() const { return frames_; }

 private:
  WavWriter() = default;
  void WriteHeader();

  std::ofstream file_;
  uint64_t frames_ = 0;
  bool open_ = false;
};

}  // namespace apu
