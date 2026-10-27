#include "xex_check.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <xex.h>

namespace xexcheck {

uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t ReadBe16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

std::optional<size_t> FindOptHeader(std::span<const uint8_t> x, uint32_t key) {
  if (x.size() < 24) return std::nullopt;
  const uint32_t count = ReadBe32(&x[20]);
  if (24 + uint64_t(count) * 8 > x.size()) return std::nullopt;
  for (uint32_t i = 0; i < count; ++i)
    if (ReadBe32(&x[24 + 8 * size_t(i)]) == key) return i;
  return std::nullopt;
}

std::optional<uint32_t> OptHeaderOffset(std::span<const uint8_t> x, uint32_t key, uint32_t min_size) {
  auto i = FindOptHeader(x, key);
  if (!i) return std::nullopt;
  const uint32_t off = ReadBe32(&x[24 + 8 * *i + 4]);
  const uint32_t header_size = ReadBe32(&x[8]);
  if (header_size > x.size()) return std::nullopt;
  if (off < 24 || uint64_t(off) + min_size > header_size) return std::nullopt;
  return off;
}

std::expected<uint64_t, std::string> ValidateXex(std::span<const uint8_t> x) {
  using std::unexpected;
  if (x.size() < 24 || std::memcmp(x.data(), "XEX2", 4) != 0)
    return unexpected("not an XEX2 file (missing magic)");
  const uint32_t header_size = ReadBe32(&x[8]);
  const uint32_t security_off = ReadBe32(&x[16]);
  const uint32_t count = ReadBe32(&x[20]);
  if (header_size > x.size() || header_size < 24)
    return unexpected("XEX header size outside the file");
  if (24 + uint64_t(count) * 8 > header_size) return unexpected("optional header table outside the headers");
  if (uint64_t(security_off) + sizeof(Xex2SecurityInfo) > header_size)
    return unexpected("security info outside the headers");
  const uint32_t image_size = ReadBe32(&x[security_off + 4]);
  if (image_size < 0x1000) return unexpected("invalid image size");

  auto ffi = OptHeaderOffset(x, XEX_HEADER_FILE_FORMAT_INFO, sizeof(Xex2OptFileFormatInfo));
  if (!ffi) return unexpected("file format info header (0x000003FF) missing or outside the headers");
  const uint32_t info_size = ReadBe32(&x[*ffi]);
  const uint16_t encryption = ReadBe16(&x[*ffi + 4]);
  const uint16_t compression = ReadBe16(&x[*ffi + 6]);
  if (uint64_t(*ffi) + info_size > header_size) return unexpected("file format info outside the headers");
  if (encryption > XEX_ENCRYPTION_NORMAL)
    return unexpected("unsupported encryption type: " + std::to_string(encryption));
  const uint64_t payload = x.size() - header_size;
  switch (compression) {
    case XEX_COMPRESSION_NONE:
      if (payload < image_size) return unexpected("uncompressed data shorter than the image");
      return uint64_t(image_size);
    case XEX_COMPRESSION_BASIC: {
      if (info_size < 8) return unexpected("file format info too short");
      const size_t blocks = info_size / 8 - 1;
      uint64_t data_total = 0, image_total = 0;
      for (size_t i = 0; i < blocks; ++i) {
        const uint8_t* b = &x[*ffi + 8 + 8 * i];
        data_total += ReadBe32(b);
        image_total += uint64_t(ReadBe32(b)) + ReadBe32(b + 4);
      }
      if (data_total > payload) return unexpected("compressed blocks past the end of the file");
      if (image_total < 0x40) return unexpected("empty compressed blocks");
      return image_total;
    }
    case XEX_COMPRESSION_NORMAL:
      if (info_size < 4 + sizeof(Xex2CompressedBlockInfo)) return unexpected("file format info too short");
      return uint64_t(image_size);
    default:
      return unexpected("unsupported compression type: " + std::to_string(compression));
  }
}

std::expected<std::vector<ImportThunk>, std::string> ReadImportThunks(std::span<const uint8_t> x) {
  using std::unexpected;
  std::vector<ImportThunk> out;
  if (!FindOptHeader(x, XEX_HEADER_IMPORT_LIBRARIES)) return out;  // no imports
  auto off = OptHeaderOffset(x, XEX_HEADER_IMPORT_LIBRARIES, sizeof(Xex2ImportHeader));
  if (!off) return unexpected("import header (0x000103FF) outside the headers");
  const uint32_t total = ReadBe32(&x[*off]);
  const uint32_t str_size = ReadBe32(&x[*off + 4]);
  const uint32_t lib_count = ReadBe32(&x[*off + 8]);
  const uint64_t end = uint64_t(*off) + total;
  const uint64_t str_begin = uint64_t(*off) + sizeof(Xex2ImportHeader);
  if (end > ReadBe32(&x[8]) || str_begin + str_size > end) return unexpected("malformed import header");

  // Name table: NUL-terminated strings, 4-byte aligned (like Xenia/XenonUtils).
  std::vector<std::string> names;
  for (uint64_t p = str_begin; p < str_begin + str_size && names.size() < lib_count;) {
    const char* s = reinterpret_cast<const char*>(&x[p]);
    const size_t len = strnlen(s, size_t(str_begin + str_size - p));
    names.emplace_back(s, len);
    p += ((len + 1) + 3) & ~uint64_t(3);
  }
  if (names.size() != lib_count) return unexpected("incomplete library name table");

  // Each library is skipped using its own `size` field (like Xenia).
  uint64_t p = str_begin + str_size;
  for (uint32_t lib = 0; lib < lib_count; ++lib) {
    if (p + sizeof(Xex2ImportLibrary) > end) return unexpected("import library outside the header");
    const uint32_t lib_size = ReadBe32(&x[p]);
    const uint16_t name_index = ReadBe16(&x[p + offsetof(Xex2ImportLibrary, name)]);
    const uint16_t record_count = ReadBe16(&x[p + offsetof(Xex2ImportLibrary, numberOfImports)]);
    if (lib_size < sizeof(Xex2ImportLibrary) + 4ull * record_count || p + lib_size > end)
      return unexpected("invalid import library size");
    if (name_index >= names.size()) return unexpected("invalid library name index");
    for (uint16_t r = 0; r < record_count; ++r)
      out.push_back({names[name_index], ReadBe32(&x[p + sizeof(Xex2ImportLibrary) + 4ull * r])});
    p += lib_size;
  }
  return out;
}

bool ZeroImportKey(std::span<uint8_t> x) {
  auto i = FindOptHeader(x, XEX_HEADER_IMPORT_LIBRARIES);
  if (!i) return false;
  std::memset(&x[24 + 8 * *i], 0, 4);
  return true;
}

namespace {

struct Export {
  uint32_t ordinal;
  const char* name;
};

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) Export{(ORDINAL), #NAME}
constexpr Export kXamExports[] = {
#include <xbox/xam_table.inc>
};
constexpr Export kKernelExports[] = {
#include <xbox/xboxkrnl_table.inc>
};
#undef XE_EXPORT

const std::unordered_map<uint32_t, const char*>& ExportTable(std::string_view module) {
  static const auto build = [](const auto& table) {
    std::unordered_map<uint32_t, const char*> m;
    for (const auto& e : table) m.emplace(e.ordinal, e.name);
    return m;
  };
  static const auto xam = build(kXamExports);
  static const auto kernel = build(kKernelExports);
  static const std::unordered_map<uint32_t, const char*> empty;
  if (module == "xam.xex") return xam;
  if (module == "xboxkrnl.exe") return kernel;
  return empty;
}

}  // namespace

std::string ExportName(std::string_view module, uint32_t ordinal) {
  const auto& table = ExportTable(module);
  if (auto it = table.find(ordinal); it != table.end()) return it->second;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "ordinal_0x%04X", ordinal);
  return buf;
}

}  // namespace xexcheck
