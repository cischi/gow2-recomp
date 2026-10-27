#include "apu/wav_writer.h"

#include <algorithm>
#include <cstring>

namespace apu {
namespace {

constexpr uint32_t kChannels = 6, kRate = 48000, kBytesPerFrame = kChannels * 4;
constexpr uint64_t kHeaderBytes = 68;
constexpr uint64_t kRewriteEvery = 48000;  // samples per channel

void Put16(uint8_t*& p, uint16_t v) {
  *p++ = uint8_t(v);
  *p++ = uint8_t(v >> 8);
}
void Put32(uint8_t*& p, uint32_t v) {
  Put16(p, uint16_t(v));
  Put16(p, uint16_t(v >> 16));
}
void PutTag(uint8_t*& p, const char* t) {
  std::memcpy(p, t, 4);
  p += 4;
}

}  // namespace

std::unique_ptr<WavWriter> WavWriter::Create(const std::filesystem::path& path) {
  std::unique_ptr<WavWriter> w(new WavWriter);
  w->file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!w->file_) return nullptr;
  w->open_ = true;
  w->WriteHeader();
  if (!w->file_) return nullptr;
  return w;
}

WavWriter::~WavWriter() { Close(); }

void WavWriter::WriteHeader() {
  const uint64_t data64 = frames_ * kBytesPerFrame;
  const uint32_t data = uint32_t(std::min<uint64_t>(data64, 0xFFFFFFFFu - 60));
  uint8_t h[kHeaderBytes];
  uint8_t* p = h;
  PutTag(p, "RIFF");
  Put32(p, data + 60);  // file - 8 = 68 - 8 + data
  PutTag(p, "WAVE");
  PutTag(p, "fmt ");
  Put32(p, 40);
  Put16(p, 0xFFFE);
  Put16(p, kChannels);
  Put32(p, kRate);
  Put32(p, kRate * kBytesPerFrame);
  Put16(p, kBytesPerFrame);
  Put16(p, 32);
  Put16(p, 22);  // cbSize
  Put16(p, 32);  // valid bits
  Put32(p, 0x3F);
  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 00000003-0000-0010-8000-00AA00389B71
  static const uint8_t kGuid[16] = {0x03, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
  std::memcpy(p, kGuid, 16);
  p += 16;
  PutTag(p, "data");
  Put32(p, data);
  file_.seekp(0);
  file_.write(reinterpret_cast<const char*>(h), sizeof(h));
  file_.seekp(0, std::ios::end);
  file_.flush();
}

void WavWriter::Append(std::span<const float> s) {
  if (!open_) return;
  size_t frames = s.size() / kChannels;
  const float* src = s.data();
  while (frames) {
    const uint64_t to_boundary = kRewriteEvery - frames_ % kRewriteEvery;
    const size_t n = size_t(std::min<uint64_t>(frames, to_boundary));
    file_.write(reinterpret_cast<const char*>(src), std::streamsize(n * kBytesPerFrame));
    frames_ += n;
    src += n * kChannels;
    frames -= n;
    if (frames_ % kRewriteEvery == 0) WriteHeader();
  }
}

void WavWriter::Close() {
  if (!open_) return;
  open_ = false;
  WriteHeader();
  file_.close();
}

}  // namespace apu
