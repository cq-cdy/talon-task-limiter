// SPDX-License-Identifier: Apache-2.0
#undef TALON_STATUS_USE_LINE_COUNTER
#define TALON_STATUS_USE_LINE_COUNTER 0
#include "talon/concurrent_task_limiter.h"

int main() { talon::ConcurrentTaskLimiter limiter(1); }
