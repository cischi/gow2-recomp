// Forwarding header in place of Xenia's third_party/fmt/include/fmt/format.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause; the submodule is not vendored): fmt::fmt from thirdparty/XenonRecomp (fmt 11) is used.
// The D3D12 backend also formats wide strings (fmt::format(L"...") for the
// debug names of D3D12 objects): in fmt 11 they live in <fmt/xchar.h>.
#pragma once

#include <fmt/format.h>
#include <fmt/xchar.h>
