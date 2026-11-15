// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "workload_support.h"

namespace {

struct ChainCase {
  int chains;
  int steps;
  int words;
  int capacity;
  int workers;
  int close_after;
};

struct Request {
  int chain;
  int step;
  std::vector<uint64_t> values;
};

struct ChainResult {
  talon::Status status;
  int completed_steps = 0;
  std::vector<uint64_t> values;
};

std::vector<uint64_t> InitialValues(int chain, int words) {
  std::vector<uint64_t> values(static_cast<std::size_t>(words));
  for (int i = 0; i < words; ++i) {
    values[i] = talon_test::MixWord(static_cast<uint64_t>(i) +
                                    UINT64_C(0x100000001b3) *
                                        static_cast<uint64_t>(chain));
  }
  return values;
}

void Transform(Request* request) {
  // A task mutates a substantial, uniquely owned buffer before handing it to
  // the next task from its capture destructor.
  for (std::size_t index = 0; index < request->values.size(); ++index) {
    uint64_t value = request->values[index];
    for (int round = 0; round < 4; ++round) {
      value = talon_test::MixWord(value + index +
                                  static_cast<uint64_t>(request->step + round));
    }
    request->values[index] = value;
  }
  if (request->step % 2 != 0) {
    std::reverse(request->values.begin(), request->values.end());
  }
}

class ChainRun {
 public:
  explicit ChainRun(ChainCase config)
      : config_(config),
        results_(static_cast<std::size_t>(config.chains)),
        finished_(static_cast<std::size_t>(config.chains)),
        visits_(static_cast<std::size_t>(config.chains * config.steps)) {
    for (auto& finished : finished_) {
      finished.store(false);
    }
    for (auto& visit : visits_) {
      visit.store(0);
    }
    for (int chain = 0; chain < config.chains; ++chain) {
      auto request = std::make_unique<Request>();
      request->chain = chain;
      request->step = 0;
      request->values = InitialValues(chain, config.words);
      ready_.push_back(std::move(request));
    }
  }

  void Attach(talon::ConcurrentTaskLimiter* limiter) { limiter_ = limiter; }
  void Dispatch();

  void Check() {
    EXPECT_EQ(invariant_failures_.load(), 0);
    EXPECT_EQ(accepted_.load(), executed_.load());
    EXPECT_EQ(accepted_.load(), destroyed_.load());
    EXPECT_EQ(terminal_.load(), config_.chains);
    if (config_.capacity == 1) {
      EXPECT_EQ(nested_accepted_.load(), 0);
      EXPECT_GT(deferred_.load(), 0);
    }
    if (config_.close_after > 0) {
      EXPECT_GE(executed_.load(), config_.close_after);
      EXPECT_LE(executed_.load(), executed_at_close_.load() + config_.capacity);
    }
    int closed_chains = 0;
    for (int chain = 0; chain < config_.chains; ++chain) {
      const auto& result = results_[chain];
      if (config_.close_after < 0) {
        EXPECT_TRUE(result.status.ok()) << result.status;
        EXPECT_EQ(result.completed_steps, config_.steps);
      } else {
        if (result.completed_steps == config_.steps) {
          EXPECT_TRUE(result.status.ok());
        } else {
          EXPECT_EQ(result.status.code(),
                    talon::StatusCode::kFailedPrecondition);
          EXPECT_LT(result.completed_steps, config_.steps);
          ++closed_chains;
        }
      }
      auto expected = InitialValues(chain, config_.words);
      // Round-major reference versus the task's element-major computation.
      for (int step = 0; step < result.completed_steps; ++step) {
        for (int round = 0; round < 4; ++round) {
          for (std::size_t index = 0; index < expected.size(); ++index) {
            expected[index] = talon_test::MixWord(
                expected[index] + index + static_cast<uint64_t>(step + round));
          }
        }
        if (step % 2 != 0) {
          std::reverse(expected.begin(), expected.end());
        }
      }
      ASSERT_EQ(result.values, expected) << "chain=" << chain;
      for (int step = 0; step < config_.steps; ++step) {
        EXPECT_EQ(visits_[chain * config_.steps + step].load(),
                  step < result.completed_steps ? 1 : 0);
      }
    }
    if (config_.close_after > 0) {
      EXPECT_GT(closed_chains, 0);
    }
  }

 private:
  class Continuation {
   public:
    Continuation(ChainRun* run, std::unique_ptr<Request> request)
        : run_(run), request_(std::move(request)) {}
    Continuation(Continuation&& other) noexcept
        : run_(other.run_),
          request_(std::move(other.request_)),
          in_executor_(true),
          ran_(std::exchange(other.ran_, false)) {}
    Continuation(const Continuation&) = delete;
    Continuation& operator=(const Continuation&) = delete;
    ~Continuation() {
      if (request_ == nullptr) {
        return;
      }
      if (in_executor_) {
        if (!ran_) {
          run_->invariant_failures_.fetch_add(1);
          run_->Finish(std::move(request_),
                       talon::DataLossError("Accepted task was not run"));
        } else {
          run_->Continue(std::move(request_));
        }
        run_->destroyed_.fetch_add(1);
      }
    }
    void operator()() && {
      ran_ = true;
      const int index = request_->chain * run_->config_.steps + request_->step;
      if (run_->visits_[index].fetch_add(1) != 0) {
        run_->invariant_failures_.fetch_add(1);
      }
      Transform(request_.get());
      ++request_->step;
      const int executed = run_->executed_.fetch_add(1) + 1;
      if (executed == run_->config_.close_after) {
        run_->limiter_->Close();
        // A worker can be preempted between reaching the threshold and Close.
        // Bound the drain from the actual close observation, not that
        // threshold.
        run_->executed_at_close_.store(run_->executed_.load());
      }
    }
    std::unique_ptr<Request> TakeRequest() { return std::move(request_); }

   private:
    ChainRun* run_;
    std::unique_ptr<Request> request_;
    bool in_executor_ = false;
    bool ran_ = false;
  };

  void Continue(std::unique_ptr<Request> request) {
    EXPECT_GE(limiter_->GetSnapshot().running, 1U);
    EXPECT_EQ(limiter_->WaitIdle().code(),
              talon::StatusCode::kFailedPrecondition);
    if (request->step == config_.steps) {
      Finish(std::move(request), talon::OkStatus());
      return;
    }
    Continuation next(this, std::move(request));
    talon::Status status = limiter_->Schedule(std::move(next));
    if (status.ok()) {
      accepted_.fetch_add(1);
      nested_accepted_.fetch_add(1);
    } else if (status.code() == talon::StatusCode::kResourceExhausted) {
      // Never spin waiting for capacity inside a worker/capture destructor.
      // Defer the still-owned payload to an external submitting thread.
      deferred_.fetch_add(1);
      Post(next.TakeRequest());
    } else {
      Finish(next.TakeRequest(), std::move(status));
    }
  }

  void Post(std::unique_ptr<Request> request) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_.push_back(std::move(request));
    }
    ready_cv_.notify_one();
  }

  void Finish(std::unique_ptr<Request> request, talon::Status status) {
    const int chain = request->chain;
    if (finished_[chain].exchange(true)) {
      invariant_failures_.fetch_add(1);
      return;
    }
    results_[chain].completed_steps = request->step;
    results_[chain].values = std::move(request->values);
    results_[chain].status = std::move(status);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      terminal_.fetch_add(1);
    }
    ready_cv_.notify_one();
  }

  ChainCase config_;
  std::vector<ChainResult> results_;
  std::vector<std::atomic<bool>> finished_;
  std::vector<std::atomic<int>> visits_;
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::deque<std::unique_ptr<Request>> ready_;
  std::atomic<int> terminal_{0};
  std::atomic<int> accepted_{0};
  std::atomic<int> executed_{0};
  std::atomic<int> executed_at_close_{0};
  std::atomic<int> destroyed_{0};
  std::atomic<int> deferred_{0};
  std::atomic<int> nested_accepted_{0};
  std::atomic<int> invariant_failures_{0};
  talon::ConcurrentTaskLimiter* limiter_ = nullptr;
};

void ChainRun::Dispatch() {
  for (;;) {
    std::unique_ptr<Request> request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!ready_cv_.wait_for(lock, std::chrono::seconds(120), [this] {
            return !ready_.empty() || terminal_.load() == config_.chains;
          })) {
        talon_test::FailSynchronization(
            "Continuation graph stopped making progress");
      }
      if (ready_.empty()) {
        return;
      }
      request = std::move(ready_.front());
      ready_.pop_front();
    }
    Continuation task(this, std::move(request));
    talon::Status status =
        talon_test::ScheduleEventually(limiter_, std::move(task));
    if (status.ok()) {
      accepted_.fetch_add(1);
    } else {
      Finish(task.TakeRequest(), std::move(status));
    }
  }
}

class DestructorContinuation : public ::testing::TestWithParam<ChainCase> {};

TEST_P(DestructorContinuation,
       CpuPayloadSurvivesDestructorResubmissionAndClose) {
  const ChainCase config = GetParam();
  ChainRun run(config);
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = config.capacity;
  options.worker_count = config.workers;
  talon::ConcurrentTaskLimiter limiter(options);
  run.Attach(&limiter);
  // The calling thread is the external dispatcher; workers remain available
  // even when a destructor's immediate attempt consumes its last free slot.
  run.Dispatch();
  EXPECT_TRUE(limiter.Shutdown().ok());
  run.Check();
}

INSTANTIATE_TEST_SUITE_P(OwnershipChains, DestructorContinuation,
                         ::testing::Values(ChainCase{4, 48, 8192, 1, 1, -1},
                                           ChainCase{8, 96, 16384, 16, 4, -1},
                                           ChainCase{8, 96, 16384, 1, 1, 80},
                                           ChainCase{8, 96, 16384, 16, 4, 80}),
                         [](const ::testing::TestParamInfo<ChainCase>& info) {
                           return "Capacity" +
                                  std::to_string(info.param.capacity) +
                                  (info.param.close_after < 0 ? "Complete"
                                                              : "CloseMidway");
                         });

}  // namespace
