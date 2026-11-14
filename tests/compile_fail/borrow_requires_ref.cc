// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  int value = 42;
  return limiter.Schedule([](int&) {}, value).ok();
}
