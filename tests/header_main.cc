// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

int RunFromOtherTranslationUnit(talon::ConcurrentTaskLimiter& limiter);

int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  const int value = RunFromOtherTranslationUnit(limiter);
  return limiter.Shutdown().ok() && value == 17 ? 0 : 1;
}
