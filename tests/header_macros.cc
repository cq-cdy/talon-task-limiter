// SPDX-License-Identifier: Apache-2.0
// Model a consumer that already has unrelated short-name status macros.
#define RETURN_IF_ERROR 913
#define ASSIGN_OR_RETURN 917

// Include order is the test: the usage requirement must precede both headers.
// clang-format off
#include "talon/status_macros.h"
#include "talon/concurrent_task_limiter.h"
// clang-format on

static_assert(RETURN_IF_ERROR == 913);
static_assert(ASSIGN_OR_RETURN == 917);
static_assert(TALON_STATUS_USE_LINE_COUNTER == 1);

int CheckMacroHeaderConsumer() {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 0;
  return talon::ConcurrentTaskLimiter::Create(options).status().code() ==
                 talon::StatusCode::kInvalidArgument
             ? 0
             : 1;
}
