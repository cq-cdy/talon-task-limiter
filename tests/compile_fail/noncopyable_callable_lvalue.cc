// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

class Callable {
 public:
  Callable() = default;
  Callable(const Callable&) = delete;
  Callable(Callable&&) = default;
  void operator()() {}
};

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  Callable callable;
  return limiter.Schedule(callable).ok();
}
