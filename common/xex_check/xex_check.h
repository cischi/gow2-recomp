// Structural checks on an XEX2 file, to run before Image::ParseImage
// (XenonUtils does not validate its input and reads out of the buffer on malformed files).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xexcheck {

uint32_t ReadBe32(const uint8_t* p);
uint16_t ReadBe16(const uint8_t* p);

// Index (in the optional header table) of the requested key.
// nullopt if absent or if the table does not fit in the buffer.
std::optional<size_t> FindOptHeader(std::span<const uint8_t> xex, uint32_t key);

// File offset of the data of a non-inline optional header, only if
// `min_size` bytes fit inside the header area.
std::optional<uint32_t> OptHeaderOffset(std::span<const uint8_t> xex, uint32_t key, uint32_t min_size);

// Validates the XEX structure. On success returns how many image bytes
// ParseImage really allocates in image.data: with "basic" compression
// it is the sum of the blocks (data+zero), which can be smaller than
// security->imageSize (GoW2: 0x1228000 versus 0x1230000) even though XenonUtils
// declares image.size = imageSize. Whoever copies image.data must copy
// min(produced, image.size) and treat the rest as zero.
// On error returns a readable message.
std::expected<uint64_t, std::string> ValidateXex(std::span<const uint8_t> xex);

// One record of the import header (0x000103FF): library and guest
// address of the descriptor (a variable slot or a function thunk).
struct ImportThunk {
  std::string module;
  uint32_t thunk;
};

// Reads the 0x000103FF header (empty vector if the XEX has no imports). The type and
// ordinal live in the image, at address `thunk` (big-endian u32:
// type in the high byte, ordinal in the low 16 bits) until ParseImage
// rewrites them: see ZeroImportKey.
std::expected<std::vector<ImportThunk>, std::string> ReadImportThunks(std::span<const uint8_t> xex);

// Zeroes the 0x000103FF key in the optional header table, so that
// ParseImage leaves the import records intact (it does not byte-swap them nor
// replace the thunks). false if the XEX has no imports.
bool ZeroImportKey(std::span<uint8_t> xex);

// Name of an xboxkrnl.exe/xam.xex export from the XenonUtils tables (the
// same ones XenonRecomp derives the __imp__ symbols from), or "ordinal_0xNNNN".
std::string ExportName(std::string_view module, uint32_t ordinal);

}  // namespace xexcheck
