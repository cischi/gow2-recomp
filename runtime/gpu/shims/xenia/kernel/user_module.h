// Shim of Xenia's src/xenia/kernel/user_module.h
// (https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// BSD-3-Clause): only title_id(), used by the CommandProcessor for trace
// file names (never opened: TraceWriter is empty).
#ifndef XENIA_KERNEL_USER_MODULE_H_
#define XENIA_KERNEL_USER_MODULE_H_

#include <cstdint>

namespace xe {
namespace kernel {

class UserModule {
 public:
  uint32_t title_id() const { return 0; }
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_USER_MODULE_H_
