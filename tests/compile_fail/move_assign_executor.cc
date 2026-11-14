// SPDX-License-Identifier: Apache-2.0
#include <utility>

#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter first(1);
  talon::ConcurrentTaskLimiter second(1);
  first = std::move(second);
}
