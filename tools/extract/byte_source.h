#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace xdvdfs {

struct ByteSource {
  virtual ~ByteSource() = default;
  virtual uint64_t size() const = 0;
  virtual bool read(uint64_t offset, void* dst, size_t len) = 0;  // false if past the end
};

std::unique_ptr<ByteSource> OpenFile(const std::filesystem::path&);  // nullptr if it cannot be opened

}  // namespace xdvdfs
