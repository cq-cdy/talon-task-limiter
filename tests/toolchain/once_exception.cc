// SPDX-License-Identifier: Apache-2.0
// Standalone diagnostic: no limiter or GoogleTest dependency. Some TSan /
// libstdc++ combinations do not reset pthread_once after exception unwinding.
#include <mutex>

int main() {
  std::once_flag flag;
  try {
    std::call_once(flag, [] { throw 1; });
  } catch (int) {
  }
  std::call_once(flag, [] {});
}
