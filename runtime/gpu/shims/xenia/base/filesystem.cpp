// Shim of Xenia's src/xenia/base/filesystem.cc and filesystem_win.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): std::filesystem and CRT stdio functions (_wfopen,
// _fseeki64, _ftelli64, _chsize_s); no OS APIs.
#include "xenia/base/filesystem.h"

#include <io.h>

#include <string>
#include <system_error>

namespace xe {

std::string path_to_utf8(const std::filesystem::path& path) { return to_utf8(path.u16string()); }

std::u16string path_to_utf16(const std::filesystem::path& path) { return path.u16string(); }

std::filesystem::path to_path(const std::string_view source) { return to_path(to_utf16(source)); }

std::filesystem::path to_path(const std::u16string_view source) {
  return std::filesystem::path(std::u16string(source));
}

namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path) {
  if (!path.has_parent_path()) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  return std::filesystem::is_directory(path.parent_path(), ec);
}

bool CreateEmptyFile(const std::filesystem::path& path) {
  FILE* file = OpenFile(path, "wb");
  if (!file) {
    return false;
  }
  std::fclose(file);
  return true;
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  const std::wstring wide_mode(mode.begin(), mode.end());
  return _wfopen(path.c_str(), wide_mode.c_str());
}

bool Seek(FILE* file, int64_t offset, int origin) { return _fseeki64(file, offset, origin) == 0; }

int64_t Tell(FILE* file) { return _ftelli64(file); }

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (std::fflush(file) != 0) {
    return false;
  }
  return _chsize_s(_fileno(file), int64_t(length)) == 0;
}

}  // namespace filesystem
}  // namespace xe
