// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "test_support.h"

namespace {
using talon::ConcurrentTaskLimiter;
using talon::StatusCode;
using talon_test::Countdown;
using talon_test::Gate;
using talon_test::OpenOnExit;

using Load = std::tuple<int, int, int>;
class Contention : public ::testing::TestWithParam<Load> {};

TEST_P(Contention, ExactlyCapacityAcceptedUnderSimultaneousAdmission) {
  const int capacity = std::get<0>(GetParam());
  const int workers = std::get<1>(GetParam());
  const int producers = std::get<2>(GetParam());
  Gate start;
  Gate release;
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = capacity;
  options.worker_count = workers;
  ConcurrentTaskLimiter limiter(options);
  OpenOnExit cleanup(release);
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::atomic<int> executions{0};
  std::vector<std::thread> threads;
  // Each producer attempts capacity times so even large queues are saturated.
  for (int i = 0; i < producers; ++i) {
    threads.emplace_back([&] {
      start.Wait();
      for (int attempt = 0; attempt < capacity; ++attempt) {
        auto status = limiter.Schedule([&] {
          release.Wait();
          ++executions;
        });
        if (status.ok()) {
          ++accepted;
        } else {
          EXPECT_EQ(status.code(), StatusCode::kResourceExhausted);
          ++rejected;
        }
      }
    });
  }
  start.Open();
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(accepted.load(), capacity);
  EXPECT_EQ(rejected.load(), capacity * (producers - 1));
  const auto snapshot = limiter.GetSnapshot();
  EXPECT_EQ(snapshot.outstanding, static_cast<std::size_t>(capacity));
  EXPECT_LE(snapshot.running, static_cast<std::size_t>(workers));
  release.Open();
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(executions.load(), capacity);
}

TEST_P(Contention, RepeatedAdmissionRunsEveryAcceptedTaskExactlyOnce) {
  const int capacity = std::get<0>(GetParam());
  const int workers = std::get<1>(GetParam());
  const int producers = std::get<2>(GetParam());
  constexpr int kTasksPerProducer = 200;
  const int total = producers * kTasksPerProducer;
  std::vector<std::atomic<int>> executions(static_cast<std::size_t>(total));
  for (auto& count : executions) count.store(0);
  std::atomic<int> active{0};
  std::atomic<int> peak{0};
  std::atomic<bool> done{false};
  Gate start;
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = capacity;
  options.worker_count = workers;
  ConcurrentTaskLimiter limiter(options);
  std::thread observer([&] {
    while (!done.load()) {
      const auto state = limiter.GetSnapshot();
      EXPECT_LE(state.outstanding, static_cast<std::size_t>(capacity));
      EXPECT_LE(state.running, static_cast<std::size_t>(workers));
      EXPECT_LE(state.queued + state.running, state.outstanding);
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> threads;
  for (int producer = 0; producer < producers; ++producer) {
    threads.emplace_back([&, producer] {
      start.Wait();
      for (int offset = 0; offset < kTasksPerProducer; ++offset) {
        const int id = producer * kTasksPerProducer + offset;
        talon_test::SpinUntil([&] {
          auto status = limiter.Schedule([&, id] {
            const int running = active.fetch_add(1) + 1;
            talon_test::UpdatePeak(peak, running);
            ++executions[static_cast<std::size_t>(id)];
            if ((id & 7) == 0) std::this_thread::yield();
            --active;
          });
          if (!status.ok()) {
            EXPECT_EQ(status.code(), StatusCode::kResourceExhausted);
          }
          return status.ok();
        });
      }
    });
  }
  start.Open();
  for (auto& thread : threads) thread.join();
  EXPECT_TRUE(limiter.WaitIdle().ok());
  done.store(true);
  observer.join();
  EXPECT_LE(peak.load(), workers);
  EXPECT_EQ(active.load(), 0);
  for (int id = 0; id < total; ++id) {
    EXPECT_EQ(executions[static_cast<std::size_t>(id)].load(), 1)
        << "id=" << id;
  }
}

INSTANTIATE_TEST_SUITE_P(Loads, Contention,
                         ::testing::Values(Load{1, 1, 8}, Load{2, 2, 16},
                                           Load{8, 2, 32}, Load{32, 8, 32},
                                           Load{64, 16, 48}));

TEST(Concurrency, CloseRacesWithProducersWithoutLossOrDuplicateExecution) {
  constexpr int kProducers = 24;
  constexpr int kAttempts = 400;
  constexpr int kTotal = kProducers * kAttempts;
  for (int round = 0; round < 8; ++round) {
    std::vector<int> admitted(kTotal, 0);
    std::vector<int> executed(kTotal, 0);
    Gate start;
    Countdown first_attempt(kProducers);
    ConcurrentTaskLimiter::Options options;
    options.max_concurrent = 64;
    options.worker_count = 8;
    ConcurrentTaskLimiter limiter(options);
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
      producers.emplace_back([&, producer] {
        start.Wait();
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
          const int id = producer * kAttempts + attempt;
          const auto status = limiter.Schedule([&, id] { ++executed[id]; });
          if (status.ok()) {
            admitted[id] = 1;
          } else {
            EXPECT_TRUE(status.code() == StatusCode::kResourceExhausted ||
                        status.code() == StatusCode::kFailedPrecondition);
          }
          if (attempt == 0) first_attempt.Arrive();
        }
      });
    }
    start.Open();
    first_attempt.Wait();
    limiter.Close();
    EXPECT_TRUE(limiter.Shutdown().ok());
    for (auto& producer : producers) producer.join();
    EXPECT_EQ(admitted, executed) << "round=" << round;
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
    EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kFailedPrecondition);
  }
}

TEST(Concurrency, ManyConcurrentShutdownCallersAllReturnAfterDrain) {
  Gate release;
  Countdown callers_started(32);
  std::atomic<int> executions{0};
  std::atomic<int> returned{0};
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 64;
  options.worker_count = 8;
  ConcurrentTaskLimiter limiter(options);
  OpenOnExit cleanup(release);
  for (int i = 0; i < 64; ++i) {
    ASSERT_TRUE(limiter
                    .Schedule([&] {
                      release.Wait();
                      ++executions;
                    })
                    .ok());
  }
  std::vector<std::thread> callers;
  for (int i = 0; i < 32; ++i) {
    callers.emplace_back([&] {
      callers_started.Arrive();
      EXPECT_TRUE(limiter.Shutdown().ok());
      EXPECT_EQ(executions.load(), 64);
      ++returned;
    });
  }
  callers_started.Wait();
  talon_test::SpinUntil([&] { return !limiter.GetSnapshot().accepting; });
  EXPECT_EQ(returned.load(), 0);
  release.Open();
  for (auto& caller : callers) caller.join();
  EXPECT_EQ(returned.load(), 32);
}

TEST(Concurrency, AllIdleWaitersWakeOnLastCompletion) {
  for (int round = 0; round < 20; ++round) {
    Gate release;
    Countdown entered(16);
    ConcurrentTaskLimiter limiter(4);
    OpenOnExit cleanup(release);
    ASSERT_TRUE(limiter.Schedule([&] { release.Wait(); }).ok());
    std::vector<std::thread> waiters;
    for (int i = 0; i < 16; ++i) {
      waiters.emplace_back([&, i] {
        entered.Arrive();
        if ((i & 1) == 0) {
          EXPECT_TRUE(limiter.WaitIdle().ok());
        } else {
          EXPECT_TRUE(limiter
                          .WaitIdleUntil(std::chrono::steady_clock::now() +
                                         std::chrono::seconds(20))
                          .ok());
        }
      });
    }
    entered.Wait();
    release.Open();
    for (auto& waiter : waiters) waiter.join();
  }
}

TEST(Concurrency, ExceptionHandlersMayOverlapAndRetainCapacity) {
  Gate release;
  Countdown entered(8);
  std::atomic<int> handled{0};
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 8;
  options.on_unhandled_exception = [&](std::exception_ptr error) {
    EXPECT_THROW(std::rethrow_exception(error), int);
    entered.Arrive();
    release.Wait();
    ++handled;
  };
  ConcurrentTaskLimiter limiter(options);
  OpenOnExit cleanup(release);
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(limiter.Schedule([i] { throw i; }).ok());
  }
  entered.Wait();
  EXPECT_EQ(limiter.GetSnapshot().running, 8U);
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kResourceExhausted);
  release.Open();
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(handled.load(), 8);
  EXPECT_TRUE(limiter.Schedule([] {}).ok());
}

TEST(Concurrency, MixedFuturesExceptionsAndCaptureFailures) {
  constexpr int kProducers = 16;
  constexpr int kTasks = 200;
  std::atomic<int> handled{0};
  std::atomic<int> expected_handlers{0};
  std::atomic<int> successes{0};
  std::atomic<int> expected_successes{0};
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 32;
  options.worker_count = 8;
  options.on_unhandled_exception = [&](std::exception_ptr error) {
    EXPECT_THROW(std::rethrow_exception(error), int);
    ++handled;
  };
  ConcurrentTaskLimiter limiter(options);
  Gate start;
  std::vector<std::thread> producers;
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      std::minstd_rand random(static_cast<unsigned>(producer + 1));
      start.Wait();
      for (int i = 0; i < kTasks; ++i) {
        const auto kind = random() % 4;
        if (kind == 0) {
          struct ThrowOnMove {
            ThrowOnMove() = default;
            ThrowOnMove(ThrowOnMove&&) { throw 11; }
            void operator()() {}
          } callable;
          try {
            const auto status = limiter.Schedule(std::move(callable));
            EXPECT_EQ(status.code(), StatusCode::kResourceExhausted);
          } catch (int error) {
            EXPECT_EQ(error, 11);
          }
        } else if (kind == 1) {
          talon_test::SpinUntil([&] {
            auto status = limiter.Schedule([] { throw 12; });
            if (status.ok()) {
              ++expected_handlers;
            } else {
              EXPECT_EQ(status.code(), StatusCode::kResourceExhausted);
            }
            return status.ok();
          });
        } else {
          talon_test::SpinUntil([&] {
            auto result = limiter.Submit([&, kind] {
              if (kind == 2) throw 13;
              ++successes;
              return 42;
            });
            if (!result.ok()) {
              EXPECT_EQ(result.status().code(), StatusCode::kResourceExhausted);
              return false;
            }
            if (kind == 2) {
              EXPECT_THROW(result->get(), int);
            } else {
              ++expected_successes;
              EXPECT_EQ(result->get(), 42);
            }
            return true;
          });
        }
      }
    });
  }
  start.Open();
  for (auto& producer : producers) producer.join();
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(handled.load(), expected_handlers.load());
  EXPECT_EQ(successes.load(), expected_successes.load());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

TEST(Concurrency, IdlePublishesNonAtomicTaskWrites) {
  constexpr int kCount = 256;
  std::array<std::uint64_t, kCount> output{};
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = kCount;
  options.worker_count = 8;
  ConcurrentTaskLimiter limiter(options);
  for (int i = 0; i < kCount; ++i) {
    const auto value = static_cast<std::uint64_t>(i) * 1234567;
    ASSERT_TRUE(limiter.Schedule([&, i, value] { output[i] = value; }).ok());
  }
  EXPECT_TRUE(limiter.WaitIdle().ok());
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(output[i], static_cast<std::uint64_t>(i) * 1234567);
  }
}

TEST(Concurrency, ManyExecutorsHaveIndependentWorkerAndReservationMarkers) {
  std::vector<std::unique_ptr<ConcurrentTaskLimiter>> limiters;
  for (int i = 0; i < 8; ++i)
    limiters.push_back(std::make_unique<ConcurrentTaskLimiter>(2));
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&, i] {
      for (int round = 0; round < 100; ++round) {
        auto& limiter = *limiters[static_cast<std::size_t>(i)];
        auto result = limiter.Submit([&] {
          EXPECT_EQ(limiter.WaitIdle().code(), StatusCode::kFailedPrecondition);
          return 42;
        });
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result->get(), 42);
        EXPECT_TRUE(limiter.WaitIdle().ok());
      }
    });
  }
  for (auto& producer : producers) producer.join();
}
}  // namespace
