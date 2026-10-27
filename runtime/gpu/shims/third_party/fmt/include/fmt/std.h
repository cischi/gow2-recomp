// Forwarding header in place of xenia-canary's third_party/fmt/include/fmt/std.h
// (@ e6cd2e4bc998609ca25e24a24c8e32b66c5908ad, submodule not
// vendored): fmt::fmt from thirdparty/XenonRecomp (fmt 11). canary's
// logging.h includes it, and with it many standard library headers that
// canary's code uses without including them.
#pragma once

#include <fmt/std.h>
