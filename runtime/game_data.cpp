#include "game_data.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#include <image.h>
#include <xex.h>

#include "xex_check.h"

namespace game_data {

namespace fs = std::filesystem;

namespace {

std::string Hex(const unsigned char (&digest)[32]) {
  std::string out;
  char hex[3];
  for (unsigned char b : digest) {
    std::snprintf(hex, sizeof(hex), "%02x", b);
    out += hex;
  }
  return out;
}

// Feeds `read` (returns bytes read into the buffer, 0 at end, -1 on error) into SHA-256.
template <class Read>
std::optional<std::string> Sha256Stream(Read read) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return std::nullopt;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::optional<std::string> result;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<unsigned char> buf(1 << 20);
    bool ok = true;
    for (;;) {
      const long got = read(buf);
      if (got < 0) ok = false;
      if (got <= 0) break;
      if (BCryptHashData(hash, buf.data(), static_cast<ULONG>(got), 0) != 0) {
        ok = false;
        break;
      }
    }
    unsigned char digest[32];
    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) result = Hex(digest);
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return result;
}

bool SkipHash() {
  char* v = nullptr;
  size_t n = 0;
  const bool skip = _dupenv_s(&v, &n, "GOW2_SKIP_XEX_HASH") == 0 && v && std::string_view(v) == "1";
  std::free(v);
  return skip;
}

}  // namespace

std::optional<std::string> Sha256File(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  return Sha256Stream([&](std::vector<unsigned char>& buf) -> long {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (in.bad() || (in.fail() && !in.eof())) return -1;
    return static_cast<long>(in.gcount());
  });
}

std::string Sha256Bytes(std::span<const std::byte> bytes) {
  size_t pos = 0;
  return Sha256Stream([&](std::vector<unsigned char>& buf) -> long {
           const size_t n = std::min(buf.size(), bytes.size() - pos);
           std::copy_n(reinterpret_cast<const unsigned char*>(bytes.data()) + pos, n, buf.begin());
           pos += n;
           return static_cast<long>(n);
         })
      .value_or(std::string());
}

std::optional<std::string> XexImageSha256(std::span<const std::byte> xex) {
  std::vector<uint8_t> file(reinterpret_cast<const uint8_t*>(xex.data()),
                            reinterpret_cast<const uint8_t*>(xex.data()) + xex.size());
  // Same steps as LoadImage and xexdump: validate first (ParseImage does not), keep the import records intact,
  // copy only the produced bytes and zero the rest.
  const auto produced = xexcheck::ValidateXex(file);
  if (!produced) return std::nullopt;
  xexcheck::ZeroImportKey(file);
  const Image image = Image::ParseImage(file.data(), file.size());
  if (!image.data || image.size < 0x40 || image.data[0] != 'M' || image.data[1] != 'Z') return std::nullopt;
  std::vector<std::byte> bytes(image.size);
  std::memcpy(bytes.data(), image.data.get(), size_t(std::min<uint64_t>(*produced, image.size)));
  return Sha256Bytes(bytes);
}

XexCheck CheckXex(const fs::path& data_dir, std::string_view expected) {
  std::error_code ec;
  const fs::path xex = data_dir / "default.xex";
  if (!fs::is_regular_file(xex, ec)) return XexCheck::kMissing;
  if (SkipHash()) return XexCheck::kOk;
  std::ifstream in(xex, std::ios::binary | std::ios::ate);
  if (!in) return XexCheck::kUnreadable;
  std::vector<char> file(size_t(in.tellg()));
  if (!in.seekg(0) || !in.read(file.data(), std::streamsize(file.size()))) return XexCheck::kUnreadable;
  const auto sum = XexImageSha256(std::as_bytes(std::span(file)));
  return sum == expected ? XexCheck::kOk : XexCheck::kMismatch;
}

bool ExtractionComplete(const fs::path& data_dir) {
  std::error_code ec;
  return fs::is_regular_file(data_dir / ".extract_ok", ec) && fs::is_regular_file(data_dir / "default.xex", ec);
}

fs::path ResolveDataDir(const fs::path& exe_dir, std::string_view dir_name) {
  fs::path dir = dir_name;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return exe_dir / dir;
  return dir;
}

}  // namespace game_data
