// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

class Result {
 public:
  ~Result() noexcept(false) {}
};

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  return limiter.Submit([] { return Result(); }).ok();
}
