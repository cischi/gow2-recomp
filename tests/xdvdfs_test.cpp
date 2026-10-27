#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "xdvdfs.h"
#include "xdvdfs_builder.h"

using namespace xdvdfs;

namespace {

struct MemSource : ByteSource {
  explicit MemSource(std::vector<uint8_t> d) : data(std::move(d)) {}
  uint64_t size() const override { return data.size(); }
  bool read(uint64_t off, void* dst, size_t len) override {
    if (off > data.size() || len > data.size() - off) return false;
    std::memcpy(dst, data.data() + off, len);
    return true;
  }
  std::vector<uint8_t> data;
};

// size() = 8 GiB; maps the image at the start and the root directory at a high offset.
struct HugeSource : ByteSource {
  HugeSource(std::vector<uint8_t> img, uint64_t dir_off, std::vector<uint8_t> dir)
      : image(std::move(img)), dir_bytes(std::move(dir)), dir_offset(dir_off) {}
  uint64_t size() const override { return 8ull << 30; }
  bool read(uint64_t off, void* dst, size_t len) override {
    if (off + len <= image.size()) {
      std::memcpy(dst, image.data() + off, len);
      return true;
    }
    if (off >= dir_offset && off + len <= dir_offset + dir_bytes.size()) {
      std::memcpy(dst, dir_bytes.data() + (off - dir_offset), len);
      return true;
    }
    return false;
  }
  std::vector<uint8_t> image, dir_bytes;
  uint64_t dir_offset;
};

uint32_t Rd32(const std::vector<uint8_t>& v, size_t at) {
  return v[at] | (v[at + 1] << 8) | (v[at + 2] << 16) | (uint32_t(v[at + 3]) << 24);
}
void Wr32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + i] = (x >> (8 * i)) & 0xFF;
}

const Entry* Find(const std::vector<Entry>& es, const std::string& p) {
  for (auto& e : es)
    if (e.path == p) return &e;
  return nullptr;
}

}  // namespace

TEST_CASE("finds the XGD2 partition") {
  MemSource s(BuildImage({{"default.xex", "XEX2"}}, 0xFD90000));
  REQUIRE(FindPartition(s).value() == 0xFD90000);
}

TEST_CASE("finds a trimmed ISO") {
  MemSource s(BuildImage({{"default.xex", "XEX2"}}, 0));
  REQUIRE(FindPartition(s).value() == 0);
}

TEST_CASE("missing signature") {
  MemSource s(std::vector<uint8_t>(0x20000));
  auto r = FindPartition(s);
  REQUIRE(!r);
  REQUIRE(r.error().message.find("MICROSOFT*XBOX*MEDIA") != std::string::npos);
}

TEST_CASE("nested listing") {
  MemSource s(BuildImage({{"default.xex", "XEX2"}, {"a/b/c.xxx", "abc"}}, 0));
  auto r = ListAll(s, 0);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 4);
  REQUIRE(Find(*r, "default.xex") != nullptr);
  REQUIRE(!Find(*r, "default.xex")->is_dir);
  REQUIRE(Find(*r, "a")->is_dir);
  REQUIRE(Find(*r, "a/b")->is_dir);
  REQUIRE(Find(*r, "a/b/c.xxx")->size == 3);
}

TEST_CASE("content is readable") {
  MemSource s(BuildImage({{"default.xex", "XEX2"}, {"a/b/c.xxx", "abc"}}, 0xFD90000));
  auto r = ListAll(s, 0xFD90000);
  REQUIRE(r.has_value());
  const std::vector<std::pair<std::string, std::string>> expected{{"default.xex", "XEX2"},
                                                                   {"a/b/c.xxx", "abc"}};
  for (auto& [p, content] : expected) {
    const Entry* e = Find(*r, p);
    REQUIRE(e);
    std::string buf(e->size, '\0');
    REQUIRE(s.read(e->offset, buf.data(), buf.size()));
    REQUIRE(buf == content);
  }
}

TEST_CASE("offset beyond 4 GB") {
  auto img = BuildImage({{"default.xex", "XEX2"}}, 0xFD90000);
  size_t vd = 0xFD90000 + 0x10000;
  uint32_t root_sector = Rd32(img, vd + 20), root_size = Rd32(img, vd + 24);
  uint64_t old_off = 0xFD90000 + uint64_t{root_sector} * 2048;
  std::vector<uint8_t> dir(img.begin() + old_off, img.begin() + old_off + root_size);
  Wr32(img, vd + 20, 0x200000);
  uint64_t dir_off = 0xFD90000 + uint64_t{0x200000} * 2048;
  REQUIRE(dir_off >= (4ull << 30));
  HugeSource s(std::move(img), dir_off, std::move(dir));
  auto r = ListAll(s, 0xFD90000);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 1);
  REQUIRE(r->front().path == "default.xex");
}

TEST_CASE("cyclic tree") {
  // Root "b" at offset 0 (16 bytes), left child "a" at dword 4: the child points to itself.
  auto img = BuildImage({{"a", "1"}, {"b", "2"}}, 0);
  uint32_t root_sector = Rd32(img, 0x10000 + 20);
  size_t child = size_t{root_sector} * 2048 + 16;
  img[child] = 4;
  img[child + 1] = 0;
  MemSource s(std::move(img));
  auto r = ListAll(s, 0);
  REQUIRE(!r);
  REQUIRE(r.error().message.find("cycle") != std::string::npos);
}

TEST_CASE("sector outside the image") {
  auto img = BuildImage({{"default.xex", "XEX2"}}, 0);
  Wr32(img, 0x10000 + 20, 0x100000);
  MemSource s(std::move(img));
  auto r = ListAll(s, 0);
  REQUIRE(!r);
  REQUIRE(r.error().message.find("outside") != std::string::npos);
}
