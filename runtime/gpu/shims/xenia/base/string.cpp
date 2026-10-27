// Shim of Xenia's src/xenia/base/string.cc
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): standard library and CRT only.
#include "xenia/base/string.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace xe {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

void AppendUtf16(std::u16string& out, char32_t cp) {
  if (cp < 0x10000) {
    out += char16_t(cp);
  } else {
    cp -= 0x10000;
    out += char16_t(0xD800 | (cp >> 10));
    out += char16_t(0xDC00 | (cp & 0x3FF));
  }
}

// Length of the UTF-8 sequence from its lead byte, 0 if it is not a lead byte.
size_t Utf8SequenceLength(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead >> 5) == 0x6) return 2;
  if ((lead >> 4) == 0xE) return 3;
  if ((lead >> 3) == 0x1E) return 4;
  return 0;
}

}  // namespace

int xe_strcasecmp(const char* string1, const char* string2) {
  return xe_strncasecmp(string1, string2, SIZE_MAX);
}

int xe_strncasecmp(const char* string1, const char* string2, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    int c1 = std::tolower(static_cast<unsigned char>(string1[i]));
    int c2 = std::tolower(static_cast<unsigned char>(string2[i]));
    if (c1 != c2 || c1 == 0) {
      return c1 - c2;
    }
  }
  return 0;
}

char* xe_strdup(const char* source) {
  size_t size = std::strlen(source) + 1;
  char* copy = static_cast<char*>(std::malloc(size));
  if (copy) {
    std::memcpy(copy, source, size);
  }
  return copy;
}

std::string to_utf8(const std::u16string_view source) {
  std::string out;
  out.reserve(source.size());
  for (size_t i = 0; i < source.size(); ++i) {
    char32_t c = source[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < source.size() && source[i + 1] >= 0xDC00 &&
        source[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (char32_t(source[i + 1]) - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = kReplacement;
    }
    AppendUtf8(out, c);
  }
  return out;
}

std::u16string to_utf16(const std::string_view source) {
  static constexpr char32_t kMinForLength[5] = {0, 0, 0x80, 0x800, 0x10000};
  std::u16string out;
  out.reserve(source.size());
  size_t i = 0;
  while (i < source.size()) {
    const unsigned char lead = static_cast<unsigned char>(source[i]);
    const size_t length = Utf8SequenceLength(lead);
    if (length == 0 || i + length > source.size()) {
      AppendUtf16(out, kReplacement);
      ++i;
      continue;
    }
    char32_t cp = length == 1 ? lead : (lead & (0x7F >> length));
    bool valid = true;
    for (size_t k = 1; k < length; ++k) {
      const unsigned char cont = static_cast<unsigned char>(source[i + k]);
      if ((cont & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }
    if (!valid || cp < kMinForLength[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      AppendUtf16(out, kReplacement);
      ++i;
      continue;
    }
    AppendUtf16(out, cp);
    i += length;
  }
  return out;
}

}  // namespace xe
