// PNG files with WIC (Windows Imaging Component, part of Windows) and the bounded write queue of the shader
// capture: the presenter converts the readback into pixels and pushes a job; a few threads encode and write it, so
// the frame is never held up by the disk. A full queue drops the job (counted) instead of blocking.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace gpu {

// 24-bit PNG from 0x00RRGGBB pixels (the input of WriteBmp24). false on any failure.
bool WritePng(const std::filesystem::path& file, uint32_t width, uint32_t height, std::span<const uint32_t> xrgb);

struct ImageJob {
  std::filesystem::path png;  // empty: no image
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> pixels;  // 0x00RRGGBB
  std::filesystem::path text;    // empty: no text
  std::string text_contents;
};

class ImageWriteQueue {
 public:
  using WriteFn = std::function<void(ImageJob&)>;  // empty: WritePng + text file
  explicit ImageWriteQueue(unsigned threads = 3, size_t capacity = 8, WriteFn write = {});
  ~ImageWriteQueue();  // writes the pending jobs, then joins
  ImageWriteQueue(const ImageWriteQueue&) = delete;
  ImageWriteQueue& operator=(const ImageWriteQueue&) = delete;

  // false: `capacity` jobs already waiting, job dropped and counted. Never blocks.
  bool Push(ImageJob job);
  // Returns when every accepted job is written.
  void Drain();
  uint64_t Dropped() const;
  uint64_t Written() const;

 private:
  void Run();

  const size_t capacity_;
  WriteFn write_;
  mutable std::mutex mutex_;
  std::condition_variable work_, idle_;
  std::deque<ImageJob> jobs_;
  size_t busy_ = 0;
  bool stop_ = false;
  uint64_t dropped_ = 0, written_ = 0;
  std::vector<std::thread> threads_;
};

}  // namespace gpu
