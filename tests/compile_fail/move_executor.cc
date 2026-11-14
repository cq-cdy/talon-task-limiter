// SPDX-License-Identifier: Apache-2.0
#include <utility>

#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  talon::ConcurrentTaskLimiter other(std::move(limiter));
}
