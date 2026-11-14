// SPDX-License-Identifier: Apache-2.0
#include <iostream>
#include <memory>

#include "talon/concurrent_task_limiter.h"

int main() {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 16;
  options.worker_count = 4;
  auto executor = talon::ConcurrentTaskLimiter::Create(std::move(options));
  if (!executor.ok()) {
    std::cerr << executor.status() << '\n';
    return 1;
  }
  auto result =
      (*executor)->Submit([](std::unique_ptr<int> value) { return *value * 2; },
                          std::make_unique<int>(21));
  if (!result.ok()) {
    std::cerr << result.status() << '\n';
    return 1;
  }
  try {
    std::cout << "Result: " << result->get() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "Task failed: " << error.what() << '\n';
    return 1;
  }
  const auto status = (*executor)->Shutdown();
  if (!status.ok()) {
    std::cerr << status << '\n';
    return 1;
  }
}
