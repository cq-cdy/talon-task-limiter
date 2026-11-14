// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

int ConsumeInAnotherTranslationUnit() {
  talon::ConcurrentTaskLimiter limiter(1);
  int result = 0;
  if (!limiter.Schedule([&] { result = 42; }).ok()) return 6;
  if (!limiter.WaitIdle().ok() || result != 42) return 7;
  return 0;
}
