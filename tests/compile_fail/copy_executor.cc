#include "talon/concurrent_task_limiter.h"
int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  talon::ConcurrentTaskLimiter copy(limiter);
}
