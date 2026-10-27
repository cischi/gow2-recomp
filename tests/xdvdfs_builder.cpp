#include "xdvdfs_builder.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace {

constexpr size_t kSector = 2048;

struct Node {
  std::string name;
  bool dir = false;
  std::string content;
  std::vector<Node> children;
};

void Put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = x & 0xFF;
  v[at + 1] = x >> 8;
}
void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + i] = (x >> (8 * i)) & 0xFF;
}

void Insert(Node& root, const std::string& path, const std::string& content) {
  Node* cur = &root;
  size_t pos = 0;
  while (true) {
    size_t slash = path.find('/', pos);
    bool last = slash == std::string::npos;
    std::string part = path.substr(pos, last ? std::string::npos : slash - pos);
    auto it = std::find_if(cur->children.begin(), cur->children.end(),
                           [&](const Node& n) { return n.name == part; });
    if (it == cur->children.end()) {
      cur->children.push_back(Node{part, !last, last ? content : std::string{}, {}});
      it = cur->children.end() - 1;
    }
    if (last) return;
    cur = &*it;
    pos = slash + 1;
  }
}

void PadSector(std::vector<uint8_t>& v, uint8_t fill) {
  v.resize((v.size() + kSector - 1) / kSector * kSector, fill);
}

// Serializes a directory: balanced binary tree, root at offset 0 (pre-order).
std::vector<uint8_t> BuildDir(const std::vector<Node>& sorted, const std::vector<uint32_t>& sectors,
                              const std::vector<uint32_t>& sizes) {
  std::vector<uint8_t> buf;
  std::function<uint16_t(int, int)> emit = [&](int lo, int hi) -> uint16_t {  // [lo,hi)
    if (lo >= hi) return 0;
    int mid = lo + (hi - lo) / 2;
    size_t at = buf.size();
    const Node& n = sorted[mid];
    buf.resize(at + (14 + n.name.size() + 3) / 4 * 4, 0xFF);
    uint16_t l = emit(lo, mid);
    uint16_t r = emit(mid + 1, hi);
    Put16(buf, at, l);
    Put16(buf, at + 2, r);
    Put32(buf, at + 4, sectors[mid]);
    Put32(buf, at + 8, sizes[mid]);
    buf[at + 12] = n.dir ? 0x10 : 0x20;
    buf[at + 13] = static_cast<uint8_t>(n.name.size());
    std::memcpy(&buf[at + 14], n.name.data(), n.name.size());
    return static_cast<uint16_t>(at / 4);
  };
  emit(0, static_cast<int>(sorted.size()));
  PadSector(buf, 0xFF);
  if (buf.empty()) buf.assign(kSector, 0xFF);
  return buf;
}

}  // namespace

std::vector<uint8_t> BuildImage(const std::vector<std::pair<std::string, std::string>>& files,
                                uint64_t partition_offset) {
  Node root;
  for (auto& [p, c] : files) Insert(root, p, c);

  // Data area: starts at sector 33 (after the volume descriptor at sector 32).
  std::vector<uint8_t> area;
  auto next_sector = [&] { return static_cast<uint32_t>(33 + area.size() / kSector); };

  std::function<std::pair<uint32_t, uint32_t>(Node&)> emit_dir = [&](Node& d) {
    std::sort(d.children.begin(), d.children.end(),
              [](const Node& a, const Node& b) { return a.name < b.name; });
    std::vector<uint32_t> sectors, sizes;
    for (Node& c : d.children) {
      if (c.dir) {
        auto [s, z] = emit_dir(c);
        sectors.push_back(s);
        sizes.push_back(z);
      } else {
        sectors.push_back(next_sector());
        sizes.push_back(static_cast<uint32_t>(c.content.size()));
        area.insert(area.end(), c.content.begin(), c.content.end());
        PadSector(area, 0);
      }
    }
    auto bytes = BuildDir(d.children, sectors, sizes);
    uint32_t s = next_sector();
    area.insert(area.end(), bytes.begin(), bytes.end());
    return std::pair<uint32_t, uint32_t>{s, static_cast<uint32_t>(bytes.size())};
  };
  auto [root_sector, root_size] = emit_dir(root);

  std::vector<uint8_t> img(partition_offset + 33 * kSector + area.size(), 0);
  uint8_t* vd = &img[partition_offset + 32 * kSector];
  static const char sig[] = "MICROSOFT*XBOX*MEDIA";
  std::memcpy(vd, sig, 20);
  std::vector<uint8_t> tmp(8);
  Put32(tmp, 0, root_sector);
  Put32(tmp, 4, root_size);
  std::memcpy(vd + 20, tmp.data(), 8);
  std::memcpy(vd + 0x7EC, sig, 20);
  std::memcpy(&img[partition_offset + 33 * kSector], area.data(), area.size());
  return img;
}
