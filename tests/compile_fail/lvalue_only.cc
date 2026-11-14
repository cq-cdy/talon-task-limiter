#include "talon/concurrent_task_limiter.h"
struct Callable {
  void operator()() & {}
};
int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  return limiter.Schedule(Callable{}).ok();
}
