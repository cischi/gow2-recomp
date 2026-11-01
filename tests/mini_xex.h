// Smallest XEX2 that xexcheck::ValidateXex and Image::ParseImage accept: no encryption, no compression, one
// 0x1000-byte image ("MZ" + `body`, no sections). `key` goes into the security info AES key field, so two
// calls with the same body and a different key are different files with the same image.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gow2_test {

inline std::string MiniXex(std::string_view body, char key = 0) {
  constexpr uint32_t kSecurity = 40, kHeaderSize = 0x200, kImageSize = 0x1000;
  std::string x(kHeaderSize + kImageSize, '\0');
  auto be32 = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) x[at + i] = char(v >> (24 - 8 * i));
  };
  x.replace(0, 4, "XEX2");
  be32(8, kHeaderSize);
  be32(16, kSecurity);
  be32(20, 1);           // one optional header:
  be32(24, 0x000003FF);  // file format info at offset 32: size 8, encryption 0, compression 0
  be32(28, 32);
  be32(32, 8);
  be32(kSecurity + 4, kImageSize);
  be32(kSecurity + 0x110, 0x82000000);  // loadAddress
  x[kSecurity + 0x150] = key;           // aesKey[0] (unused without encryption)
  x.replace(kHeaderSize, 2, "MZ");
  x.replace(kHeaderSize + 0x40, body.size(), body);
  return x;
}

}  // namespace gow2_test
