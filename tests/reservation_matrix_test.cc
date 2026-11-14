// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "test_support.h"

namespace {

using talon::ConcurrentTaskLimiter;
using talon::StatusCode;

struct ReservationCase {
  int count;
  int workers;
  int failure_pattern;
  bool submit;
  bool close;
};

struct ConstructionState {
  talon_test::Gate permit;
  talon_test::Gate finished;
  std::atomic<int> calls{0};
  std::atomic<int> copies_destroyed{0};
};

class ControlledCopy {
 public:
  ControlledCopy(ConstructionState* state, talon_test::Countdown* started,
                 bool fail)
      : state_(state), started_(started), fail_(fail) {}

  ControlledCopy(const ControlledCopy& other)
      : state_(other.state_),
        started_(other.started_),
        fail_(other.fail_),
        is_copy_(true) {
    started_->Arrive();
    state_->permit.Wait();
    if (fail_) {
      // Deliberate fault injection; this is not a library error path.
      throw 17;
    }
  }

  ~ControlledCopy() {
    if (is_copy_) {
      state_->copies_destroyed.fetch_add(1);
    }
  }

  int operator()() && {
    state_->calls.fetch_add(1);
    return 42;
  }

 private:
  ConstructionState* state_;
  talon_test::Countdown* started_;
  bool fail_;
  bool is_copy_ = false;
};

bool ShouldFail(int index, int pattern) {
  return pattern == 2 || (pattern == 1 && (index % 2) == 0);
}

std::vector<ReservationCase> ReservationCases() {
  std::vector<ReservationCase> cases;
  for (const auto& size : std::array<std::array<int, 2>, 4>{
           std::array<int, 2>{1, 1}, {8, 1}, {8, 4}, {32, 8}}) {
    for (int pattern = 0; pattern < 3; ++pattern) {
      for (bool submit : {false, true}) {
        for (bool close : {false, true}) {
          cases.push_back({size[0], size[1], pattern, submit, close});
        }
      }
    }
  }
  return cases;
}

class ReservationMatrix : public ::testing::TestWithParam<ReservationCase> {};

TEST_P(ReservationMatrix,
       ReversePublicationAndRollbackKeepAllWaitersConsistent) {
  const ReservationCase config = GetParam();
  std::vector<std::unique_ptr<ConstructionState>> states;
  for (int i = 0; i < config.count; ++i) {
    states.push_back(std::make_unique<ConstructionState>());
  }
  talon_test::Countdown constructing(static_cast<std::size_t>(config.count));
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = config.count;
  options.worker_count = config.workers;
  ConcurrentTaskLimiter limiter(options);
  std::vector<std::thread> producers;
  std::atomic<int> accepted{0};
  std::atomic<int> rolled_back{0};
  for (int i = 0; i < config.count; ++i) {
    producers.emplace_back([&, i] {
      ControlledCopy callable(states[i].get(), &constructing,
                              ShouldFail(i, config.failure_pattern));
      try {
        if (config.submit) {
          auto result = limiter.Submit(callable);
          EXPECT_TRUE(result.ok()) << result.status();
          if (result.ok()) {
            accepted.fetch_add(1);
            EXPECT_EQ(result->get(), 42);
          }
        } else {
          const auto status = limiter.Schedule(callable);
          EXPECT_TRUE(status.ok()) << status;
          if (status.ok()) {
            accepted.fetch_add(1);
          }
        }
      } catch (int error) {
        EXPECT_EQ(error, 17);
        EXPECT_TRUE(ShouldFail(i, config.failure_pattern));
        rolled_back.fetch_add(1);
      }
      states[i]->finished.Open();
    });
  }
  constructing.Wait();
  const auto reserved = limiter.GetSnapshot();
  EXPECT_EQ(reserved.outstanding, static_cast<std::size_t>(config.count));
  EXPECT_EQ(reserved.running, 0U);
  EXPECT_EQ(reserved.queued, 0U);
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kResourceExhausted);
  if (config.close) {
    limiter.Close();
  }
  EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
            StatusCode::kDeadlineExceeded);

  constexpr int kWaiters = 8;
  talon_test::Countdown waiting(kWaiters);
  std::vector<std::thread> waiters;
  std::atomic<int> returned{0};
  for (int i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&, i] {
      waiting.Arrive();
      if (config.close && (i % 2) == 0) {
        EXPECT_TRUE(limiter.Shutdown().ok());
      } else {
        EXPECT_TRUE(limiter.WaitIdle().ok());
      }
      returned.fetch_add(1);
    });
  }
  waiting.Wait();
  // Keep reservation 0 unpublished until every other submission has completed.
  // No idle waiter may return early, even if the queue is completely empty.
  for (int i = config.count - 1; i > 0; --i) {
    states[i]->permit.Open();
    states[i]->finished.Wait();
  }
  EXPECT_EQ(returned.load(), 0);
  EXPECT_GE(limiter.GetSnapshot().outstanding, 1U);
  states[0]->permit.Open();
  for (auto& producer : producers) {
    producer.join();
  }
  for (auto& waiter : waiters) {
    waiter.join();
  }
  EXPECT_EQ(returned.load(), kWaiters);
  EXPECT_EQ(accepted.load() + rolled_back.load(), config.count);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  for (int i = 0; i < config.count; ++i) {
    const int expected = ShouldFail(i, config.failure_pattern) ? 0 : 1;
    EXPECT_EQ(states[i]->calls.load(), expected) << "task=" << i;
    EXPECT_EQ(states[i]->copies_destroyed.load(), expected) << "task=" << i;
  }
  if (config.close) {
    EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kFailedPrecondition);
  } else {
    EXPECT_TRUE(limiter.Schedule([] {}).ok());
    EXPECT_TRUE(limiter.WaitIdle().ok());
  }
  EXPECT_TRUE(limiter.Shutdown().ok());
}

INSTANTIATE_TEST_SUITE_P(
    Reservations, ReservationMatrix, ::testing::ValuesIn(ReservationCases()),
    [](const ::testing::TestParamInfo<ReservationCase>& info) {
      const auto& value = info.param;
      return "Capacity" + std::to_string(value.count) + "Workers" +
             std::to_string(value.workers) + "Fault" +
             std::to_string(value.failure_pattern) +
             (value.submit ? "Submit" : "Schedule") +
             (value.close ? "Closed" : "Open");
    });

}  // namespace
