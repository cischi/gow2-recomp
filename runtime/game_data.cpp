#include "game_data.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>


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

XexCheck CheckXex(const fs::path& data_dir, std::string_view expected) {
  std::error_code ec;
  const fs::path xex = data_dir / "default.xex";
  if (!fs::is_regular_file(xex, ec)) return XexCheck::kMissing;
  if (SkipHash()) return XexCheck::kOk;
  const auto sum = Sha256File(xex);
  if (!sum) return XexCheck::kUnreadable;
  return *sum == expected ? XexCheck::kOk : XexCheck::kMismatch;
}

bool ExtractionComplete(const fs::path& data_dir) {
  std::error_code ec;
  return fs::is_regular_file(data_dir / ".extract_ok", ec) && fs::is_regular_file(data_dir / "default.xex", ec);
}

fs::path ResolveDataDir(const fs::path& exe_dir) {
  fs::path dir = "game_data";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return exe_dir / "game_data";
  return dir;
}

}  // namespace game_data
