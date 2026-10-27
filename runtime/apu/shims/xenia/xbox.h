// Shim for Xenia's src/xenia/xbox.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): xma_context.h includes it only for xenia/base/memory.h
// (xe::copy_and_swap) and xenia/base/assert.h (static_assert_size), both
// vendored. No X_* type is needed by the vendored APU.
#ifndef XENIA_XBOX_H_
#define XENIA_XBOX_H_

#include "xenia/base/assert.h"
#include "xenia/base/memory.h"

#endif  // XENIA_XBOX_H_
