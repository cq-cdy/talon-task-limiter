// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  return limiter.Schedule([](int, int) {}, 42).ok();
}
