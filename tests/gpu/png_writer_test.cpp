// PNG writer with WIC and bounded write queue (gpu/png_writer.h), without a GPU.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/png_writer.h"
#include "temp_dir.h"

namespace {

using Microsoft::WRL::ComPtr;

// Pixels of a PNG as 0x00RRGGBB, decoded by WIC as 32bppBGR.
std::vector<uint32_t> DecodePng(const std::filesystem::path& file, uint32_t& w, uint32_t& h) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> factory;
  REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
  ComPtr<IWICBitmapDecoder> decoder;
  REQUIRE(SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)));
  GUID format{};
  REQUIRE(SUCCEEDED(decoder->GetContainerFormat(&format)));
  CHECK(format == GUID_ContainerFormatPng);
  ComPtr<IWICBitmapFrameDecode> frame;
  REQUIRE(SUCCEEDED(decoder->GetFrame(0, &frame)));
  ComPtr<IWICFormatConverter> conv;
  REQUIRE(SUCCEEDED(factory->CreateFormatConverter(&conv)));
  REQUIRE(SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)));
  REQUIRE(SUCCEEDED(conv->GetSize(&w, &h)));
  std::vector<uint32_t> px(size_t(w) * h);
  REQUIRE(SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data()))));
  for (uint32_t& p : px) p &= 0x00FFFFFF;
  return px;
}

}  // namespace

TEST_CASE("png_writer: WritePng round-trips through WIC", "[png_writer]") {
  gow2_test::TempDir tmp("png_writer");
  const std::vector<uint32_t> px{0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF, 0x00000000, 0x00123456};
  const auto file = tmp.path / "a.png";
  REQUIRE(gpu::WritePng(file, 3, 2, px));
  uint32_t w = 0, h = 0;
  CHECK(DecodePng(file, w, h) == px);
  CHECK(w == 3);
  CHECK(h == 2);
}

TEST_CASE("png_writer: WritePng into a missing folder fails", "[png_writer]") {
  gow2_test::TempDir tmp("png_writer");
  const std::vector<uint32_t> px{0};
  CHECK_FALSE(gpu::WritePng(tmp.path / "missing" / "a.png", 1, 1, px));
}

TEST_CASE("png_writer: default queue writes the PNG and the text", "[png_writer]") {
  gow2_test::TempDir tmp("png_writer");
  {
    gpu::ImageWriteQueue q;
    gpu::ImageJob job;
    job.png = tmp.path / "1.png";
    job.width = 1;
    job.height = 1;
    job.pixels = {0x00ABCDEF};
    job.text = tmp.path / "1.txt";
    job.text_contents = "frame 00001\n";
    REQUIRE(q.Push(std::move(job)));
    q.Drain();
    CHECK(q.Written() == 1);
  }
  uint32_t w = 0, h = 0;
  CHECK(DecodePng(tmp.path / "1.png", w, h) == std::vector<uint32_t>{0x00ABCDEF});
  std::ifstream f(tmp.path / "1.txt", std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(f), {}) == "frame 00001\n");
}

TEST_CASE("png_writer: full queue drops and counts without blocking", "[png_writer]") {
  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  std::atomic<int> started{0};
  gpu::ImageWriteQueue q(1, 2, [&](gpu::ImageJob&) {
    ++started;
    std::unique_lock l(m);
    cv.wait(l, [&] { return release; });
  });
  REQUIRE(q.Push({}));
  while (started.load() == 0) std::this_thread::yield();  // the thread holds the first job
  CHECK(q.Push({}));
  CHECK(q.Push({}));
  CHECK_FALSE(q.Push({}));
  CHECK(q.Dropped() == 1);
  {
    std::lock_guard l(m);
    release = true;
  }
  cv.notify_all();
  q.Drain();
  CHECK(q.Written() == 3);
}

TEST_CASE("png_writer: destroying the queue writes the pending jobs", "[png_writer]") {
  std::atomic<int> written{0};
  {
    gpu::ImageWriteQueue q(2, 8, [&](gpu::ImageJob&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      ++written;
    });
    for (int i = 0; i < 5; ++i) REQUIRE(q.Push({}));
  }
  CHECK(written.load() == 5);
}
