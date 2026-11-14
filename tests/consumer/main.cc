// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "talon/concurrent_task_limiter.h"

int ConsumeInAnotherTranslationUnit();

int main() {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 2;
  auto limiter = talon::ConcurrentTaskLimiter::Create(options);
  if (!limiter.ok()) return 1;
  auto future =
      (*limiter)->Submit([](std::unique_ptr<int> input) { return *input; },
                         std::make_unique<int>(42));
  if (!future.ok() || future->get() != 42) return 2;
  if (!(*limiter)->Shutdown().ok()) return 3;
  if ((*limiter)->Schedule([] {}).code() !=
      talon::StatusCode::kFailedPrecondition)
    return 4;
  options.max_concurrent = 0;
  if (talon::ConcurrentTaskLimiter::Create(options).status().code() !=
      talon::StatusCode::kInvalidArgument)
    return 5;
  return ConsumeInAnotherTranslationUnit();
}
