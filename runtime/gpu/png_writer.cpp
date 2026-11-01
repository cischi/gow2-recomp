// See png_writer.h.
#include "gpu/png_writer.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fstream>

#include "log.h"

namespace gpu {

namespace {

using Microsoft::WRL::ComPtr;

void WriteJob(ImageJob& job) {
  if (!job.png.empty() && !WritePng(job.png, job.width, job.height, job.pixels))
    LogLine("[gpu] shader capture: PNG not written: " + job.png.string());
  if (!job.text.empty()) {
    std::ofstream f(job.text, std::ios::binary);
    f << job.text_contents;
  }
}

}  // namespace

bool WritePng(const std::filesystem::path& file, uint32_t width, uint32_t height, std::span<const uint32_t> xrgb) {
  if (!width || !height || xrgb.size() < size_t(width) * height) return false;
  // Every writer thread initializes COM for itself; a thread already in STA keeps it (RPC_E_CHANGED_MODE).
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICBitmap> bitmap;
    // 0x00RRGGBB little-endian = bytes B, G, R, 0: WIC's 32bppBGR; WriteSource converts to the PNG's 24bppBGR.
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(width, height)) &&
         SUCCEEDED(factory->CreateBitmapFromMemory(
             width, height, GUID_WICPixelFormat32bppBGR, width * 4, UINT(size_t(width) * height * 4),
             reinterpret_cast<BYTE*>(const_cast<uint32_t*>(xrgb.data())), &bitmap)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}

ImageWriteQueue::ImageWriteQueue(unsigned threads, size_t capacity, WriteFn write)
    : capacity_(capacity), write_(write ? std::move(write) : WriteFn(&WriteJob)) {
  for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { Run(); });
}

ImageWriteQueue::~ImageWriteQueue() {
  Drain();
  {
    std::lock_guard g(mutex_);
    stop_ = true;
  }
  work_.notify_all();
  for (std::thread& t : threads_) t.join();
}

bool ImageWriteQueue::Push(ImageJob job) {
  {
    std::lock_guard g(mutex_);
    if (jobs_.size() >= capacity_) {
      ++dropped_;
      return false;
    }
    jobs_.push_back(std::move(job));
  }
  work_.notify_one();
  return true;
}

void ImageWriteQueue::Drain() {
  std::unique_lock l(mutex_);
  idle_.wait(l, [this] { return jobs_.empty() && !busy_; });
}

uint64_t ImageWriteQueue::Dropped() const {
  std::lock_guard g(mutex_);
  return dropped_;
}

uint64_t ImageWriteQueue::Written() const {
  std::lock_guard g(mutex_);
  return written_;
}

void ImageWriteQueue::Run() {
  std::unique_lock l(mutex_);
  for (;;) {
    work_.wait(l, [this] { return stop_ || !jobs_.empty(); });
    if (jobs_.empty()) return;  // stop_ with nothing left
    ImageJob job = std::move(jobs_.front());
    jobs_.pop_front();
    ++busy_;
    l.unlock();
    write_(job);
    l.lock();
    --busy_;
    ++written_;
    if (jobs_.empty() && !busy_) idle_.notify_all();
  }
}

}  // namespace gpu
