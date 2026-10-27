// See bmp.h.
#include "gpu/bmp.h"

#include <fstream>
#include <string>
#include <vector>

namespace gpu {

namespace {

void Put16(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

}  // namespace

bool WriteBmp24(const std::filesystem::path& file, uint32_t width, uint32_t height,
                std::span<const uint32_t> pixels) {
  if (width == 0 || height == 0 || width > 0x8000 || height > 0x8000 ||
      pixels.size() != size_t(width) * height)
    return false;
  const uint32_t pitch = (width * 3 + 3) & ~3u;
  const uint32_t image_size = pitch * height;
  constexpr uint32_t kHeaderSize = 14 + 40;
  std::vector<uint8_t> out(kHeaderSize + size_t(image_size), 0);
  uint8_t* h = out.data();
  h[0] = 'B';
  h[1] = 'M';
  Put32(h + 2, kHeaderSize + image_size);  // bfSize
  Put32(h + 10, kHeaderSize);              // bfOffBits
  Put32(h + 14, 40);                       // biSize
  Put32(h + 18, width);
  Put32(h + 22, height);  // positive: bottom-up
  Put16(h + 26, 1);       // biPlanes
  Put16(h + 28, 24);      // biBitCount
  Put32(h + 30, 0);       // BI_RGB
  Put32(h + 34, image_size);
  Put32(h + 38, 2835);  // 72 dpi
  Put32(h + 42, 2835);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t* src = pixels.data() + size_t(height - 1 - y) * width;
    uint8_t* dst = out.data() + kHeaderSize + size_t(y) * pitch;
    for (uint32_t x = 0; x < width; ++x) {
      dst[x * 3 + 0] = uint8_t(src[x]);
      dst[x * 3 + 1] = uint8_t(src[x] >> 8);
      dst[x * 3 + 2] = uint8_t(src[x] >> 16);
    }
  }
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  f.close();
  return !f.fail();
}

bool WritePpm10(const std::filesystem::path& file, uint32_t width, uint32_t height,
                std::span<const uint32_t> pixels) {
  if (width == 0 || height == 0 || width > 0x8000 || height > 0x8000 ||
      pixels.size() != size_t(width) * height)
    return false;
  const std::string head = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n1023\n";
  std::vector<uint8_t> out(head.begin(), head.end());
  out.resize(head.size() + size_t(width) * height * 6);
  uint8_t* p = out.data() + head.size();
  for (const uint32_t v : pixels) {
    for (const uint32_t shift : {0u, 10u, 20u}) {
      const uint32_t c = (v >> shift) & 0x3FF;
      p[0] = uint8_t(c >> 8);  // big-endian, as required by P6
      p[1] = uint8_t(c);
      p += 2;
    }
  }
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  f.close();
  return !f.fail();
}

}  // namespace gpu
