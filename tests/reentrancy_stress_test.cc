// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
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

struct NestingCase {
  int depth;
  bool close;
  bool fail;
};

class NestedCapture {
 public:
  NestedCapture(ConcurrentTaskLimiter* limiter, int remaining, bool close,
                bool fail, std::atomic<int>* calls)
      : limiter_(limiter),
        remaining_(remaining),
        close_(close),
        fail_(fail),
        calls_(calls) {}

  NestedCapture(const NestedCapture& other)
      : limiter_(other.limiter_),
        remaining_(other.remaining_),
        close_(other.close_),
        fail_(other.fail_),
        calls_(other.calls_) {
    EXPECT_EQ(limiter_->WaitIdle().code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(limiter_->Shutdown().code(), StatusCode::kFailedPrecondition);
    if (remaining_ > 1) {
      NestedCapture next(limiter_, remaining_ - 1, close_, fail_, calls_);
      EXPECT_TRUE(limiter_->Schedule(next).ok());
    } else {
      const auto snapshot = limiter_->GetSnapshot();
      EXPECT_EQ(snapshot.outstanding,
                static_cast<std::size_t>(snapshot.max_concurrent));
      EXPECT_EQ(snapshot.queued, 0U);
      EXPECT_EQ(snapshot.running, 0U);
      if (close_) {
        limiter_->Close();
      }
      if (fail_) {
        throw 23;
      }
    }
  }

  void operator()() && { calls_->fetch_add(1); }

 private:
  ConcurrentTaskLimiter* limiter_;
  int remaining_;
  bool close_;
  bool fail_;
  std::atomic<int>* calls_;
};

std::vector<NestingCase> NestingCases() {
  std::vector<NestingCase> cases;
  for (int depth : {1, 2, 32, 128}) {
    for (bool close : {false, true}) {
      for (bool fail : {false, true}) {
        cases.push_back({depth, close, fail});
      }
    }
  }
  return cases;
}

class NestedReservations : public ::testing::TestWithParam<NestingCase> {};

TEST_P(NestedReservations, DeepUnwindingRestoresEveryThreadLocalReservation) {
  const NestingCase config = GetParam();
  std::atomic<int> calls{0};
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = config.depth;
  options.worker_count = std::min(config.depth, 4);
  ConcurrentTaskLimiter limiter(options);
  NestedCapture callable(&limiter, config.depth, config.close, config.fail,
                         &calls);
  if (config.fail) {
    EXPECT_THROW(static_cast<void>(limiter.Schedule(callable)), int);
  } else {
    EXPECT_TRUE(limiter.Schedule(callable).ok());
  }
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  EXPECT_EQ(calls.load(), config.fail ? 0 : config.depth);
  if (!config.close) {
    EXPECT_TRUE(limiter.Schedule([&] { calls.fetch_add(1); }).ok());
  }
  EXPECT_TRUE(limiter.Shutdown().ok());
  EXPECT_EQ(calls.load(),
            (config.fail ? 0 : config.depth) + (config.close ? 0 : 1));
}

INSTANTIATE_TEST_SUITE_P(Depths, NestedReservations,
                         ::testing::ValuesIn(NestingCases()),
                         [](const ::testing::TestParamInfo<NestingCase>& info) {
                           return "Depth" + std::to_string(info.param.depth) +
                                  (info.param.close ? "Closed" : "Open") +
                                  (info.param.fail ? "Rollback" : "Publish");
                         });

class LifetimeToken {
 public:
  explicit LifetimeToken(std::atomic<int>* destroyed) : destroyed_(destroyed) {}
  ~LifetimeToken() { destroyed_->fetch_add(1); }
  LifetimeToken(const LifetimeToken&) = delete;
  LifetimeToken& operator=(const LifetimeToken&) = delete;

 private:
  std::atomic<int>* destroyed_;
};

struct GraphCase {
  int branching;
  int depth;
  bool submit;
};

class TaskGraph : public ::testing::TestWithParam<GraphCase> {};

TEST_P(TaskGraph, CrossExecutorFanOutHasExactlyOnceExecutionAndDestruction) {
  const GraphCase config = GetParam();
  int total = 1;
  int width = 1;
  for (int level = 0; level < config.depth; ++level) {
    width *= config.branching;
    total += width;
  }
  std::vector<std::atomic<int>> visits(static_cast<std::size_t>(total));
  for (auto& visit : visits) {
    visit.store(0);
  }
  std::vector<std::future<int>> futures(static_cast<std::size_t>(total));
  std::atomic<int> remaining{total};
  std::atomic<int> destroyed{0};
  talon_test::Gate start;
  talon_test::Gate complete;
  std::vector<std::unique_ptr<ConcurrentTaskLimiter>> pools;
  for (int i = 0; i < 3; ++i) {
    ConcurrentTaskLimiter::Options options;
    options.max_concurrent = total;
    options.worker_count = 4;
    pools.push_back(std::make_unique<ConcurrentTaskLimiter>(options));
  }
  std::function<void(int)> dispatch;
  dispatch = [&](int id) {
    auto task = [&, id, token = std::make_unique<LifetimeToken>(&destroyed)] {
      start.Wait();
      EXPECT_EQ(visits[id].fetch_add(1), 0) << "node=" << id;
      for (int offset = 1; offset <= config.branching; ++offset) {
        const int child = id * config.branching + offset;
        if (child < total) {
          dispatch(child);
        }
      }
      if (remaining.fetch_sub(1) == 1) {
        complete.Open();
      }
      return id;
    };
    ConcurrentTaskLimiter& pool = *pools[static_cast<std::size_t>(id % 3)];
    if (config.submit) {
      auto future = pool.Submit(std::move(task));
      if (!future.ok()) {
        talon_test::FailSynchronization("Unexpected graph admission failure");
      }
      futures[id] = std::move(*future);
    } else {
      if (!pool.Schedule(std::move(task)).ok()) {
        talon_test::FailSynchronization("Unexpected graph admission failure");
      }
    }
  };
  dispatch(0);
  start.Open();
  complete.Wait();
  // All parent publications have returned before the last invocation decrements
  // remaining. No task can publish new work while these executors are joined.
  for (auto& pool : pools) {
    EXPECT_TRUE(pool->Shutdown().ok());
  }
  EXPECT_EQ(destroyed.load(), total);
  for (int id = 0; id < total; ++id) {
    EXPECT_EQ(visits[id].load(), 1) << "node=" << id;
    if (config.submit) {
      ASSERT_TRUE(futures[id].valid());
      EXPECT_EQ(futures[id].get(), id);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Graphs, TaskGraph,
    ::testing::Values(GraphCase{2, 8, false}, GraphCase{2, 8, true},
                      GraphCase{3, 6, false}, GraphCase{3, 6, true},
                      GraphCase{3, 7, false}, GraphCase{3, 7, true}));

class OwnedInvocation {
 public:
  OwnedInvocation(int id, std::atomic<int>* visits, std::atomic<int>* destroyed)
      : id_(id),
        visits_(visits),
        token_(std::make_unique<LifetimeToken>(destroyed)) {}
  OwnedInvocation(OwnedInvocation&&) = default;
  OwnedInvocation(const OwnedInvocation&) = delete;
  OwnedInvocation& operator=(const OwnedInvocation&) = delete;
  bool has_payload() const { return token_ != nullptr; }
  int operator()() && {
    visits_->fetch_add(1);
    return id_;
  }

 private:
  int id_;
  std::atomic<int>* visits_;
  std::unique_ptr<LifetimeToken> token_;
};

TEST(Stress, QuarterMillionMoveOnlyTasksPreserveOwnershipAcrossRejection) {
  constexpr int kProducers = 64;
  constexpr int kPerProducer = 4096;
  constexpr int kTotal = kProducers * kPerProducer;
  std::vector<std::atomic<int>> visits(kTotal);
  for (auto& visit : visits) {
    visit.store(0);
  }
  std::atomic<int> destroyed{0};
  talon_test::Gate start;
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 256;
  options.worker_count = 8;
  ConcurrentTaskLimiter limiter(options);
  std::vector<std::thread> producers;
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      start.Wait();
      for (int offset = 0; offset < kPerProducer; ++offset) {
        const int id = producer * kPerProducer + offset;
        OwnedInvocation callable(id, &visits[id], &destroyed);
        talon_test::SpinUntil([&] {
          talon::Status status;
          if (id % 4 == 0) {
            auto result = limiter.Submit(std::move(callable));
            status = result.status();
            if (result.ok()) {
              EXPECT_EQ(result->get(), id);
            }
          } else {
            status = limiter.Schedule(std::move(callable));
          }
          if (!status.ok()) {
            EXPECT_EQ(status.code(), StatusCode::kResourceExhausted);
            EXPECT_TRUE(callable.has_payload());
          }
          return status.ok();
        });
        EXPECT_FALSE(callable.has_payload());
      }
    });
  }
  start.Open();
  for (auto& producer : producers) {
    producer.join();
  }
  EXPECT_TRUE(limiter.Shutdown().ok());
  EXPECT_EQ(destroyed.load(), kTotal);
  for (int id = 0; id < kTotal; ++id) {
    EXPECT_EQ(visits[id].load(), 1) << "task=" << id;
  }
}

}  // namespace
