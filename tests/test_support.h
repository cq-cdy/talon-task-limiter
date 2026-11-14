// SPDX-License-Identifier: Apache-2.0
#ifndef TALON_TASK_LIMITER_TESTS_TEST_SUPPORT_H_
#define TALON_TASK_LIMITER_TESTS_TEST_SUPPORT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace talon_test {
using namespace std::chrono_literals;

// Test synchronization uses predicates, not scheduling assumptions or sleeps.
// A bounded wait turns most broken protocols into a useful failure, not a hang.
class Gate {
 public:
  void Open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, 20s, [this] { return open_; })) {
      throw std::runtime_error("Test gate timed out");
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

class OpenOnExit {
 public:
  explicit OpenOnExit(Gate& gate) : gate_(gate) {}
  ~OpenOnExit() { gate_.Open(); }
  OpenOnExit(const OpenOnExit&) = delete;
  OpenOnExit& operator=(const OpenOnExit&) = delete;

 private:
  Gate& gate_;
};

class Countdown {
 public:
  explicit Countdown(std::size_t count) : count_(count) {}
  void Arrive() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) throw std::logic_error("Countdown underflow");
    if (--count_ == 0) cv_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, 20s, [this] { return count_ == 0; })) {
      throw std::runtime_error("Test countdown timed out");
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t count_;
};

inline void UpdatePeak(std::atomic<int>& peak, int current) {
  int observed = peak.load(std::memory_order_relaxed);
  while (observed < current &&
         !peak.compare_exchange_weak(observed, current,
                                     std::memory_order_relaxed)) {
  }
}

template <typename Predicate>
void SpinUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("Test condition timed out");
    }
    std::this_thread::yield();
  }
}
}  // namespace talon_test
#endif  // TALON_TASK_LIMITER_TESTS_TEST_SUPPORT_H_
