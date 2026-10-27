#pragma once
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "byte_source.h"

namespace xdvdfs {

struct Entry {
  std::string path;  // relative, '/' separator
  uint64_t offset;   // absolute offset in the image
  uint32_t size;
  bool is_dir;
};
struct Error {
  std::string message;
};

// Tries the known partition offsets (XGD2, XGD3, XGD1, trimmed ISO) and returns the first valid one.
std::expected<uint64_t, Error> FindPartition(ByteSource&);
std::expected<std::vector<Entry>, Error> ListAll(ByteSource&, uint64_t partition);

}  // namespace xdvdfs
