#include "talon/concurrent_task_limiter.h"
int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  return limiter.Schedule(42).ok();
}
