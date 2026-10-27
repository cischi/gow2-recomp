#include "image_loader.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <image.h>
#include <xex.h>

#include "log.h"
#include "platform/platform.h"
#include "xex_check.h"

// xex.h includes windows.h, which defines LoadImage as a macro (LoadImageA/W):
// the symbol must stay the one declared in image_loader.h.
#undef LoadImage

namespace {

std::string Show(const std::filesystem::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::unexpected<LoadError> Fail(LoadError e, const std::filesystem::path& xex, const std::string& why) {
  LogLine("LoadImage: " + Show(xex) + ": " + why);
  return std::unexpected(e);
}

// Optional header with an inline value (key with low byte 0, e.g.
// XEX_HEADER_SYSTEM_FLAGS, XEX_HEADER_DEFAULT_STACK_SIZE). All fields are
// big-endian; 0 if absent or the header is truncated.
uint32_t ReadInlineOptHeader(const std::vector<uint8_t>& xex, uint32_t key) {
  if (xex.size() < sizeof(Xex2Header)) return 0;
  const auto* h = reinterpret_cast<const Xex2Header*>(xex.data());
  const uint64_t count = h->headerCount;
  if (sizeof(Xex2Header) + count * sizeof(Xex2OptHeader) > xex.size()) return 0;
  const auto* opt = reinterpret_cast<const Xex2OptHeader*>(xex.data() + sizeof(Xex2Header));
  for (uint64_t i = 0; i < count; ++i)
    if (opt[i].key == key) return opt[i].value;
  return 0;
}

}  // namespace

std::expected<LoadedImage, LoadError> LoadImage(GuestMemory& mem, const std::filesystem::path& xex) {
  if (!mem.base) return Fail(LoadError::ParseFailed, xex, "guest memory not initialized");
  auto file = plat::ReadWholeFile(xex);
  if (!file) return Fail(LoadError::FileMissing, xex, "file missing or unreadable");

  // ParseImage does not validate its input: first the structural checks shared with xexdump.
  // `produced` = bytes ParseImage really allocates (can be < image.size).
  const auto produced = xexcheck::ValidateXex(*file);
  if (!produced) return Fail(LoadError::ParseFailed, xex, produced.error());

  // Import records read before taking them away from ParseImage, which would
  // otherwise rewrite them in the image (swapped bytes, replaced thunks).
  auto thunks = xexcheck::ReadImportThunks(*file);
  if (!thunks) return Fail(LoadError::ParseFailed, xex, thunks.error());
  const uint32_t system_flags = ReadInlineOptHeader(*file, XEX_HEADER_SYSTEM_FLAGS);
  const uint32_t default_stack_size = ReadInlineOptHeader(*file, XEX_HEADER_DEFAULT_STACK_SIZE);
  // Original header (with the import key): ValidateXex has already
  // checked the structure, headerSize is bounded by the file size.
  const uint32_t header_size = reinterpret_cast<const Xex2Header*>(file->data())->headerSize;
  std::vector<uint8_t> header(file->begin(), file->begin() + std::min<size_t>(header_size, file->size()));
  xexcheck::ZeroImportKey(*file);

  const Image image = Image::ParseImage(file->data(), file->size());
  if (!image.data || image.size < 0x40 || image.data[0] != 'M' || image.data[1] != 'Z')
    return Fail(LoadError::ParseFailed, xex, "decryption/decompression failed (image is not PE)");
  // The image must leave at least 64 KB of guest space for the heap.
  const uint64_t end = uint64_t(image.base) + image.size;
  const uint64_t heap = (end + 0xFFFF) & ~uint64_t(0xFFFF);
  if (image.base == 0 || heap >= GuestMemory::kSize)
    return Fail(LoadError::ParseFailed, xex, "image outside guest space");
  if (image.entry_point < image.base || image.entry_point >= end)
    return Fail(LoadError::ParseFailed, xex, "entry point outside the image");

  // The arena is only reserved: the image pages are committed.
  if (!mem.Commit(uint32_t(image.base), uint32_t(image.size)))
    return Fail(LoadError::ParseFailed, xex, "committing the image pages failed");

  // Copies only the produced bytes; the rest of the image is zero (as in Xenia).
  const size_t copied = size_t(std::min<uint64_t>(*produced, image.size));
  uint8_t* dst = mem.base + image.base;
  std::memcpy(dst, image.data.get(), copied);
  std::memset(dst + copied, 0, image.size - copied);

  std::vector<ImportRecord> imports;
  imports.reserve(thunks->size());
  for (const auto& t : *thunks) {
    if (t.thunk < image.base || uint64_t(t.thunk) + 4 > end)
      return Fail(LoadError::ParseFailed, xex, "import record outside the image");
    const uint8_t* p = dst + (t.thunk - image.base);
    const uint32_t value = xexcheck::ReadBe32(p);
    imports.push_back({t.module, t.thunk, value & 0xFFFF, value >> 24});
  }

  mem.SetHeapStart(uint32_t(heap));
  return LoadedImage{uint32_t(image.base), image.size, uint32_t(image.entry_point), system_flags, default_stack_size,
                     std::move(header), std::move(imports)};
}
