// Shim of Xenia's src/xenia/base/string.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): same functions, with the UTF-8/UTF-16 conversions written here
// (Xenia uses utfcpp, not vendored).
#ifndef XENIA_BASE_STRING_H_
#define XENIA_BASE_STRING_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace xe {

int xe_strcasecmp(const char* string1, const char* string2);
int xe_strncasecmp(const char* string1, const char* string2, size_t count);
char* xe_strdup(const char* source);

// Invalid sequences become U+FFFD.
std::string to_utf8(const std::u16string_view source);
std::u16string to_utf16(const std::string_view source);

}  // namespace xe

#endif  // XENIA_BASE_STRING_H_
