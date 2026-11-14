// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

int RunFromOtherTranslationUnit(talon::ConcurrentTaskLimiter& limiter) {
  auto result = limiter.Submit([] { return 17; });
  return result.ok() ? result->get() : -1;
}
