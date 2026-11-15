// SPDX-License-Identifier: Apache-2.0
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

#include "talon/concurrent_task_limiter.h"
#include "talon/status_macros.h"

namespace {
talon::StatusOr<int> RunExample() {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 16;
  options.worker_count = 4;
  TALON_ASSIGN_OR_RETURN(
      auto executor, talon::ConcurrentTaskLimiter::Create(std::move(options)));
  TALON_ASSIGN_OR_RETURN(
      auto future,
      executor->Submit([](std::unique_ptr<int> value) { return *value * 2; },
                       std::make_unique<int>(21)));
  const int value = future.get();
  TALON_RETURN_IF_ERROR(executor->Shutdown());
  return value;
}
}  // namespace

int main() {
  try {
    auto result = RunExample();
    if (!result.ok()) {
      std::cerr << result.status() << '\n';
      return 1;
    }
    std::cout << "Result: " << *result << '\n';
  } catch (const std::exception& error) {
    std::cerr << "Task failed: " << error.what() << '\n';
    return 1;
  }
}
