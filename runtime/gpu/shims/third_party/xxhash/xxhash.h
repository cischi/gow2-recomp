// Forwarding header in place of Xenia's third_party/xxhash/xxhash.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause; the submodule is not vendored): xxHash::xxhash from thirdparty/XenonRecomp is used
// (xxHash 0.8; xenia/base/xxhash.h defines XXH_INLINE_ALL before including it).
#pragma once

#include <xxhash.h>
