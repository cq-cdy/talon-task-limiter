// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

class Result {
 public:
  Result() = default;
  Result(const Result&) = delete;
  Result(Result&&) = delete;
};

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  return limiter.Submit([] { return Result(); }).ok();
}
