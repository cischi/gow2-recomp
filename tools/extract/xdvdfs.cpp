#include "xdvdfs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

namespace xdvdfs {
namespace {

constexpr uint64_t kSector = 2048;
constexpr uint64_t kDescriptorOffset = 32 * kSector;  // +0x10000
constexpr char kSignature[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kSigLen = 20;
constexpr uint64_t kTailSigOffset = 0x7EC;
constexpr uint64_t kPartitions[] = {0xFD90000, 0x2080000, 0x18300000, 0};

std::string Hex(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

class FileSource : public ByteSource {
 public:
  FileSource(std::ifstream f, uint64_t size) : f_(std::move(f)), size_(size) {}
  uint64_t size() const override { return size_; }
  bool read(uint64_t offset, void* dst, size_t len) override {
    if (offset > size_ || len > size_ - offset) return false;
    f_.clear();
    f_.seekg(static_cast<std::streamoff>(offset));
    f_.read(static_cast<char*>(dst), static_cast<std::streamsize>(len));
    return static_cast<size_t>(f_.gcount()) == len;
  }

 private:
  std::ifstream f_;
  uint64_t size_;
};

bool HasSignatures(ByteSource& src, uint64_t partition) {
  uint8_t buf[kSigLen];
  uint64_t base = partition + kDescriptorOffset;
  if (!src.read(base, buf, kSigLen) || std::memcmp(buf, kSignature, kSigLen) != 0) return false;
  return src.read(base + kTailSigOffset, buf, kSigLen) && std::memcmp(buf, kSignature, kSigLen) == 0;
}

struct DirJob {
  std::string prefix;  // "" or "a/b/"
  uint64_t offset;
  uint32_t size;
};

}  // namespace

std::unique_ptr<ByteSource> OpenFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return nullptr;
  f.seekg(0, std::ios::end);
  auto end = f.tellg();
  if (end < 0) return nullptr;
  return std::make_unique<FileSource>(std::move(f), static_cast<uint64_t>(end));
}

std::expected<uint64_t, Error> FindPartition(ByteSource& src) {
  for (uint64_t p : kPartitions)
    if (HasSignatures(src, p)) return p;
  return std::unexpected(
      Error{"MICROSOFT*XBOX*MEDIA signature not found at any known partition offset"});
}

std::expected<std::vector<Entry>, Error> ListAll(ByteSource& src, uint64_t partition) {
  if (!HasSignatures(src, partition))
    return std::unexpected(
        Error{"MICROSOFT*XBOX*MEDIA signature missing at partition " + Hex(partition)});

  uint8_t vd[32];
  if (!src.read(partition + kDescriptorOffset, vd, sizeof vd))
    return std::unexpected(Error{"volume descriptor outside the image"});
  uint32_t root_sector = U32(vd + 20), root_size = U32(vd + 24);

  std::vector<Entry> out;
  std::vector<DirJob> jobs;
  jobs.push_back({"", partition + uint64_t{root_sector} * kSector, root_size});
  std::set<uint64_t> seen;  // absolute offsets of entries already visited (cycle detection)

  while (!jobs.empty()) {
    DirJob job = std::move(jobs.back());
    jobs.pop_back();
    if (job.size == 0) continue;
    if (job.offset > src.size() || job.size > src.size() - job.offset)
      return std::unexpected(
          Error{"directory at offset " + Hex(job.offset) + " outside the image"});
    std::vector<uint8_t> dir(job.size);
    if (!src.read(job.offset, dir.data(), dir.size()))
      return std::unexpected(
          Error{"directory read at offset " + Hex(job.offset) + " outside the image"});

    // true = valid entry to visit, false = padding/empty directory.
    auto enter = [&](uint32_t off) -> std::expected<bool, Error> {
      if (uint64_t{off} + 14 > dir.size())
        return std::unexpected(
            Error{"directory entry at offset " + Hex(job.offset + off) + " outside the directory"});
      if (U16(&dir[off]) == 0xFFFF) return false;
      if (uint64_t{off} + 14 + dir[off + 13] > dir.size())
        return std::unexpected(
            Error{"entry name at offset " + Hex(job.offset + off) + " outside the directory"});
      if (!seen.insert(job.offset + off).second)
        return std::unexpected(
            Error{"cycle in the directory tree at offset " + Hex(job.offset + off)});
      return true;
    };

    // In-order visit (left, node, right) with an explicit stack. Child offset 0 = no child.
    std::vector<uint32_t> stack;  // byte offset inside the directory
    uint32_t cur = 0;
    auto first = enter(0);
    if (!first) return std::unexpected(first.error());
    bool have_cur = *first;
    while (have_cur || !stack.empty()) {
      if (have_cur) {
        stack.push_back(cur);
        uint16_t left = U16(&dir[cur]);
        if (left == 0) {
          have_cur = false;
        } else {
          cur = uint32_t{left} * 4;
          auto r = enter(cur);
          if (!r) return std::unexpected(r.error());
          have_cur = *r;
        }
        continue;
      }
      const uint8_t* e = &dir[stack.back()];
      stack.pop_back();
      uint32_t sector = U32(e + 4), size = U32(e + 8);
      bool is_dir = (e[12] & 0x10) != 0;
      Entry ent{job.prefix + std::string(reinterpret_cast<const char*>(e + 14), e[13]),
                partition + uint64_t{sector} * kSector, size, is_dir};
      if (is_dir) jobs.push_back({ent.path + "/", ent.offset, size});
      out.push_back(std::move(ent));

      uint16_t right = U16(e + 2);
      if (right == 0) {
        have_cur = false;
      } else {
        cur = uint32_t{right} * 4;
        auto r = enter(cur);
        if (!r) return std::unexpected(r.error());
        have_cur = *r;
      }
    }
  }
  return out;
}

}  // namespace xdvdfs
