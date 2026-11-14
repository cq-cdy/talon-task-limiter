#include "talon/concurrent_task_limiter.h"
struct Callable {
  void operator()() {}
  ~Callable() noexcept(false) {}
};
int main() {
  talon::ConcurrentTaskLimiter limiter(1);
  return limiter.Schedule(Callable{}).ok();
}
