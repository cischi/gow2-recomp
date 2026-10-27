// Shim of Xenia's src/xenia/base/filesystem.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): the functions used by the GPU code, on std::filesystem and
// the CRT's stdio files.
#ifndef XENIA_BASE_FILESYSTEM_H_
#define XENIA_BASE_FILESYSTEM_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "xenia/base/platform.h"
#include "xenia/base/string.h"

namespace xe {

std::string path_to_utf8(const std::filesystem::path& path);
std::u16string path_to_utf16(const std::filesystem::path& path);
std::filesystem::path to_path(const std::string_view source);
std::filesystem::path to_path(const std::u16string_view source);

namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path);
bool CreateEmptyFile(const std::filesystem::path& path);
// mode as in fopen ("rb", "a+b"...); nullptr if the file cannot be opened.
FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode);
bool Seek(FILE* file, int64_t offset, int origin);
int64_t Tell(FILE* file);
// Resizes the file to `length` bytes (after flushing the buffers).
bool TruncateStdioFile(FILE* file, uint64_t length);

}  // namespace filesystem
}  // namespace xe

#endif  // XENIA_BASE_FILESYSTEM_H_
