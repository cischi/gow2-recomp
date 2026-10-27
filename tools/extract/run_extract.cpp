#include "run_extract.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include "xdvdfs.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kMarkerName = ".extract_ok";
constexpr size_t kChunk = 4u << 20;  // 4 MiB

fs::path FromUtf8(const std::string& s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}

// The path comes from arbitrary bytes on the disc: validate it before touching the filesystem.
bool IsSafeEntryPath(const std::string& path) {
  if (path.empty() || path.front() == '/') return false;
  size_t pos = 0;
  while (true) {
    size_t slash = path.find('/', pos);
    std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (part.empty() || part == "." || part == "..") return false;
    if (part.find_first_of("\\:") != std::string::npos) return false;
    if (std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 0x20; })) return false;
    if (slash == std::string::npos) return true;
    pos = slash + 1;
  }
}

}  // namespace

int RunExtract(const fs::path& iso, const fs::path& out, std::ostream& log, const ExtractHooks& hooks) {
  auto src = xdvdfs::OpenFile(iso);
  if (!src) {
    log << "error: cannot open " << iso.string() << "\n";
    return 1;
  }
  auto part = xdvdfs::FindPartition(*src);
  if (!part) {
    log << "error: " << iso.string() << ": " << part.error().message << "\n";
    return 1;
  }
  auto entries = xdvdfs::ListAll(*src, *part);
  if (!entries) {
    log << "error: " << iso.string() << ": " << entries.error().message << "\n";
    return 1;
  }
  // Validate everything before writing anything.
  for (const auto& e : *entries) {
    if (!IsSafeEntryPath(e.path)) {
      log << "error: unsafe path in the image, rejected: '" << e.path << "'\n";
      return 1;
    }
  }

  std::error_code ec;
  fs::create_directories(out, ec);
  if (ec) {
    log << "error: cannot create " << out.string() << ": " << ec.message() << "\n";
    return 1;
  }

  // An extraction in progress is not complete: drop the marker of a previous one.
  fs::remove(out / kMarkerName, ec);

  uint64_t total = 0;
  for (const auto& e : *entries) total += e.is_dir ? 0 : e.size;
  uint64_t written = 0;
  auto cancelled = [&] { return hooks.cancelled && hooks.cancelled(); };

  std::vector<char> buf(kChunk);
  size_t count = 0;
  for (const auto& e : *entries) {
    fs::path dest = out / FromUtf8(e.path);
    if (e.is_dir) {
      fs::create_directories(dest, ec);
      if (ec) {
        log << "error: cannot create " << e.path << ": " << ec.message() << "\n";
        return 1;
      }
      continue;
    }
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
      log << "error: cannot create the folder of " << e.path << ": " << ec.message() << "\n";
      return 1;
    }
    // A half-written file must not stay behind: it would look like a valid extraction.
    auto discard = [&](std::ofstream& s) {
      s.close();
      std::error_code rec;
      fs::remove(dest, rec);
    };
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) {
      log << "error: cannot write " << e.path << "\n";
      return 1;
    }
    uint64_t done = 0;
    while (done < e.size) {
      if (cancelled()) {
        discard(f);
        log << "extraction cancelled\n";
        return 3;
      }
      size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, e.size - done));
      if (!src->read(e.offset + done, buf.data(), n)) {
        log << "error: read failed for " << e.path << " at offset " << (e.offset + done) << "\n";
        discard(f);
        return 1;
      }
      f.write(buf.data(), static_cast<std::streamsize>(n));
      if (!f) {
        log << "error: write failed for " << e.path << "\n";
        discard(f);
        return 1;
      }
      done += n;
      written += n;
      if (hooks.progress) hooks.progress(written, total);
    }
    f.close();
    if (!f) {
      log << "error: close failed for " << e.path << "\n";
      std::error_code rec;
      fs::remove(dest, rec);
      return 1;
    }
    ++count;
    if (cancelled()) {
      log << "extraction cancelled\n";
      return 3;
    }
  }
  if (hooks.progress) hooks.progress(total, total);
  // Last action: the marker tells users of out that the extraction is complete.
  {
    std::ofstream m(out / kMarkerName, std::ios::binary | std::ios::trunc);
    m << count << "\n";
    m.close();
    if (!m) {
      log << "error: cannot write the marker " << kMarkerName << "\n";
      return 1;
    }
  }
  log << count << " files extracted\n";
  return 0;
}

std::optional<uint64_t> IsoPayloadBytes(const fs::path& iso) {
  auto src = xdvdfs::OpenFile(iso);
  if (!src) return std::nullopt;
  auto part = xdvdfs::FindPartition(*src);
  if (!part) return std::nullopt;
  auto entries = xdvdfs::ListAll(*src, *part);
  if (!entries) return std::nullopt;
  uint64_t total = 0;
  for (const auto& e : *entries) total += e.is_dir ? 0 : e.size;
  return total;
}

std::optional<std::vector<std::byte>> ReadIsoFile(const fs::path& iso, std::string_view name) {
  auto src = xdvdfs::OpenFile(iso);
  if (!src) return std::nullopt;
  auto part = xdvdfs::FindPartition(*src);
  if (!part) return std::nullopt;
  auto entries = xdvdfs::ListAll(*src, *part);
  if (!entries) return std::nullopt;
  for (const auto& e : *entries) {
    if (e.is_dir || e.path != name) continue;
    std::vector<std::byte> data(e.size);
    if (e.size && !src->read(e.offset, data.data(), e.size)) return std::nullopt;
    return data;
  }
  return std::nullopt;
}
