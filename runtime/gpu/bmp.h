// 24-bit BMP (Global Constraints: 24-bit BMP screenshots), without OS APIs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace gpu {

// Writes `file` as an uncompressed 24-bit BMP (BITMAPINFOHEADER, bottom-up
// rows aligned to 4 bytes, BGR pixels) from width*height 0x00RRGGBB pixels
// (the high byte is ignored) ordered from the top row. Returns false if the
// dimensions are zero or do not match the pixels, or if the file cannot be written.
bool WriteBmp24(const std::filesystem::path& file, uint32_t width, uint32_t height,
                std::span<const uint32_t> xrgb8888_top_down);

// Writes `file` as a 16-bit binary PPM (P6), maxval 1023, big-endian, rows
// from the top, from width*height R10G10B10A2_UNORM pixels (r in bits 0-9, alpha
// ignored). Same error rules as WriteBmp24.
bool WritePpm10(const std::filesystem::path& file, uint32_t width, uint32_t height,
                std::span<const uint32_t> r10g10b10a2_top_down);

}  // namespace gpu
