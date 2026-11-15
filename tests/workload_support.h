// SPDX-License-Identifier: Apache-2.0
#ifndef TALON_TASK_LIMITER_TESTS_WORKLOAD_SUPPORT_H_
#define TALON_TASK_LIMITER_TESTS_WORKLOAD_SUPPORT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "talon/concurrent_task_limiter.h"
#include "talon/status_macros.h"
#include "test_support.h"

namespace talon_test {

// Retry belongs to the test's external producers, never to an executor worker.
// Reuse the same move-only callable: rejected admission must not consume it.
template <typename Func>
talon::Status ScheduleEventually(talon::ConcurrentTaskLimiter* limiter,
                                 Func&& task,
                                 std::atomic<int>* rejections = nullptr) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    talon::Status status = limiter->Schedule(std::forward<Func>(task));
    if (status.code() != talon::StatusCode::kResourceExhausted) {
      return status;
    }
    if (rejections != nullptr) {
      rejections->fetch_add(1, std::memory_order_relaxed);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return talon::DeadlineExceededError("Producer could not obtain capacity");
    }
    std::this_thread::yield();
  }
}

template <typename Func>
auto SubmitEventually(talon::ConcurrentTaskLimiter* limiter, Func&& task,
                      std::atomic<int>* rejections = nullptr)
    -> decltype(limiter->Submit(std::forward<Func>(task))) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    auto result = limiter->Submit(std::forward<Func>(task));
    if (result.status().code() != talon::StatusCode::kResourceExhausted) {
      return result;
    }
    if (rejections != nullptr) {
      rejections->fetch_add(1, std::memory_order_relaxed);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return talon::DeadlineExceededError("Producer could not obtain capacity");
    }
    std::this_thread::yield();
  }
}

class JoinThreads {
 public:
  explicit JoinThreads(std::vector<std::thread>* threads) : threads_(threads) {}
  JoinThreads(const JoinThreads&) = delete;
  JoinThreads& operator=(const JoinThreads&) = delete;
  ~JoinThreads() {
    for (auto& thread : *threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

 private:
  std::vector<std::thread>* threads_;
};

class DrainExecutors {
 public:
  explicit DrainExecutors(
      const std::vector<std::unique_ptr<talon::ConcurrentTaskLimiter>>* pools)
      : pools_(pools) {}
  DrainExecutors(const DrainExecutors&) = delete;
  DrainExecutors& operator=(const DrainExecutors&) = delete;
  ~DrainExecutors() {
    // Keep every executor object alive until cross-pool publishers have
    // returned, including when the test leaves through an exception/assertion.
    for (const auto& pool : *pools_) {
      pool->Close();
    }
    for (const auto& pool : *pools_) {
      static_cast<void>(pool->Shutdown());
    }
  }

 private:
  const std::vector<std::unique_ptr<talon::ConcurrentTaskLimiter>>* pools_;
};

// Integer-only work keeps the oracle exact on every supported platform.
inline uint64_t MixWord(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

template <typename T>
void RequireReady(std::future<T>* future) {
  if (future->wait_for(std::chrono::seconds(120)) !=
      std::future_status::ready) {
    FailSynchronization("Workload did not produce its terminal result");
  }
}

}  // namespace talon_test

#endif  // TALON_TASK_LIMITER_TESTS_WORKLOAD_SUPPORT_H_
