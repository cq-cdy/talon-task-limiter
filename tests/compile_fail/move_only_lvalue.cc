// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  auto argument = std::make_unique<int>(42);
  return limiter.Schedule([](std::unique_ptr<int>) {}, argument).ok();
}
