#include "talon/concurrent_task_limiter.h"
int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  int value = 42;
  return limiter.Submit([&]() -> int&& { return std::move(value); }).ok();
}
