// SPDX-License-Identifier: Apache-2.0
#include <functional>

#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  const int value = 42;
  return limiter.Schedule([](int&) {}, std::cref(value)).ok();
}
