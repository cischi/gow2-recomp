// xexdump <xex> <out_dir>
//
// Decrypts and decompresses an XEX2 with XenonUtils' Image::ParseImage and writes:
//   out_dir/image.bin  PE image (without the XEX header), `size` bytes long
//   out_dir/info.json  {"base","size","entry","imports":[{"module","name","thunk",
//                       "ordinal","kind"}]}
//
// The imports are read from the 0x000103FF optional header, because ParseImage
// only exposes the names of known functions (as symbols) and rewrites the import
// records in the image. To keep the original records in image.bin, ParseImage
// gets a copy of the XEX in which the 0x000103FF key is zeroed.
//
// Exit 0 on success, 1 with a message on stderr if loading fails,
// 2 for incorrect usage.

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <image.h>
#include <xex.h>

#include "xex_check.h"

namespace fs = std::filesystem;
using xexcheck::ReadBe32;

namespace {

struct Error {
  std::string message;
};

std::optional<std::vector<uint8_t>> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return std::nullopt;
  return data;
}

struct Import {
  std::string module;
  std::string name;
  uint32_t thunk;
  uint32_t ordinal;
  std::string kind;
};

// Import records from the 0x000103FF header; type and ordinal come from the records
// in the image (not rewritten by ParseImage, see xexcheck::ZeroImportKey).
std::optional<Error> ReadImports(const std::vector<uint8_t>& x, uint32_t base,
                                 const std::vector<uint8_t>& pe, std::vector<Import>& out) {
  auto thunks = xexcheck::ReadImportThunks(x);
  if (!thunks) return Error{thunks.error()};
  for (const auto& t : *thunks) {
    if (t.thunk < base || uint64_t(t.thunk) - base + 4 > pe.size())
      return Error{"import record outside the image: " + std::to_string(t.thunk)};
    const uint32_t value = ReadBe32(&pe[t.thunk - base]);
    const uint32_t type = value >> 24;
    const uint32_t ordinal = value & 0xFFFF;
    const char* kind = type == XEX_THUNK_VARIABLE   ? "variable"
                       : type == XEX_THUNK_FUNCTION ? "function"
                                                    : "unknown";
    out.push_back({t.module, xexcheck::ExportName(t.module, ordinal), t.thunk, ordinal, kind});
  }
  return std::nullopt;
}

std::string JsonString(std::string_view s) {
  std::string r = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      r += '\\';
      r += char(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      r += buf;
    } else {
      r += char(c);
    }
  }
  return r + "\"";
}

// UTF-8 path for messages (string() throws on characters outside the code page).
std::string Show(const fs::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

int Fail(const std::string& message) {
  std::cerr << "xexdump: " << message << "\n";
  return 1;
}

int Run(const fs::path& xex_path, const fs::path& out_dir) {
  auto xex = ReadFile(xex_path);
  if (!xex) return Fail("cannot read " + Show(xex_path));
  // `produced`: image bytes that ParseImage really allocates (see xex_check.h).
  const auto produced = xexcheck::ValidateXex(*xex);
  if (!produced) return Fail(Show(xex_path) + ": " + produced.error());

  // Copy with the import key zeroed: ParseImage leaves the records alone.
  std::vector<uint8_t> copy = *xex;
  xexcheck::ZeroImportKey(copy);

  Image image = Image::ParseImage(copy.data(), copy.size());
  if (!image.data || image.size < 0x40 || image.data[0] != 'M' || image.data[1] != 'Z')
    return Fail(Show(xex_path) + ": decryption/decompression failed (not a PE image)");

  // Full image of image.size bytes: the part not produced by the loader is
  // zero, as in guest memory (same semantics as Xenia).
  std::vector<uint8_t> pe(image.size, 0);
  std::memcpy(pe.data(), image.data.get(), size_t(std::min<uint64_t>(*produced, image.size)));
  if (image.entry_point < image.base || image.entry_point >= image.base + image.size)
    return Fail("entry point outside the image");

  std::vector<Import> imports;
  if (auto err = ReadImports(*xex, uint32_t(image.base), pe, imports)) return Fail(Show(xex_path) + ": " + err->message);

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) return Fail("cannot create " + Show(out_dir) + ": " + ec.message());

  {
    std::ofstream bin(out_dir / "image.bin", std::ios::binary | std::ios::trunc);
    bin.write(reinterpret_cast<const char*>(pe.data()), std::streamsize(pe.size()));
    if (!bin) return Fail("writing image.bin failed in " + Show(out_dir));
  }
  {
    std::ostringstream js;
    js << "{\n  \"base\": " << image.base << ",\n  \"size\": " << image.size
       << ",\n  \"entry\": " << image.entry_point << ",\n  \"imports\": [";
    for (size_t i = 0; i < imports.size(); ++i) {
      const auto& im = imports[i];
      js << (i ? ",\n" : "\n") << "    {\"module\": " << JsonString(im.module)
         << ", \"name\": " << JsonString(im.name) << ", \"thunk\": " << im.thunk
         << ", \"ordinal\": " << im.ordinal << ", \"kind\": " << JsonString(im.kind) << "}";
    }
    js << "\n  ]\n}\n";
    std::ofstream info(out_dir / "info.json", std::ios::binary | std::ios::trunc);
    info << js.str();
    if (!info) return Fail("writing info.json failed in " + Show(out_dir));
  }

  std::cout << "base 0x" << std::hex << image.base << " size 0x" << image.size << " entry 0x"
            << image.entry_point << std::dec << ", " << imports.size() << " imports -> "
            << Show(out_dir) << "\n";
  return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if (argc != 3) {
    std::cerr << "usage: xexdump <xex> <out_dir>\n";
    return 2;
  }
  return Run(fs::path(argv[1]), fs::path(argv[2]));
}
