// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <future>
#include <memory>
#include <new>
#include <utility>

#include "allocation_failure.h"
#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "test_support.h"

namespace {
using talon::ConcurrentTaskLimiter;
using talon_test::FailAllocation;

TEST(Allocation, SuccessfulScheduleAllocatesExactlyOneNode) {
  ConcurrentTaskLimiter limiter(1);
  bool accepted = false;
  int remaining = -1;
  {
    FailAllocation fail(1);
    accepted = limiter.Schedule([] {}).ok();
    remaining = FailAllocation::Remaining();
  }
  EXPECT_TRUE(accepted);
  EXPECT_FALSE(FailAllocation::Failed());
  EXPECT_EQ(remaining, 0);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Allocation, TaskNodeFailureReturnsReservation) {
  ConcurrentTaskLimiter limiter(1);
  bool caught = false;
  {
    FailAllocation fail(0);
    try {
      static_cast<void>(limiter.Schedule([] {}));
    } catch (const std::bad_alloc&) {
      caught = true;
    }
  }
  EXPECT_TRUE(caught);
  EXPECT_TRUE(FailAllocation::Failed());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  EXPECT_TRUE(limiter.Schedule([] {}).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Allocation, EveryFutureConstructionAllocationRollsBack) {
  ConcurrentTaskLimiter limiter(1);
  int failures = 0;
  bool reached_success = false;
  for (int allocation = 0; allocation < 16; ++allocation) {
    bool caught = false;
    std::future<int> future;
    {
      FailAllocation fail(allocation);
      try {
        auto result = limiter.Submit([] { return 42; });
        if (result.ok()) {
          future = std::move(*result);
        }
      } catch (const std::bad_alloc&) {
        caught = true;
      }
    }
    EXPECT_TRUE(limiter.WaitIdle().ok());
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
    if (caught) {
      EXPECT_TRUE(FailAllocation::Failed());
      ++failures;
    } else {
      ASSERT_TRUE(future.valid());
      EXPECT_EQ(future.get(), 42);
      reached_success = true;
      break;
    }
  }
  EXPECT_GE(failures, 2);
  EXPECT_TRUE(reached_success);
}

TEST(Allocation, ConstructorFailureJoinsPartiallyStartedWorkers) {
  int failures = 0;
  bool reached_success = false;
  for (int allocation = 0; allocation < 32; ++allocation) {
    bool caught = false;
    {
      FailAllocation fail(allocation);
      try {
        ConcurrentTaskLimiter limiter(4);
      } catch (const std::bad_alloc&) {
        caught = true;
      }
    }
    if (caught) {
      EXPECT_TRUE(FailAllocation::Failed());
      ++failures;
    } else {
      reached_success = true;
      break;
    }
  }
  EXPECT_GE(failures, 5);
  EXPECT_TRUE(reached_success);
}

TEST(Allocation, FactoryMapsAllocationFailureToResourceExhausted) {
  ConcurrentTaskLimiter::Options options;
  talon::StatusCode code = talon::StatusCode::kOk;
  {
    FailAllocation fail(0);
    auto result = ConcurrentTaskLimiter::Create(options);
    code = result.status().code();
  }
  EXPECT_EQ(code, talon::StatusCode::kResourceExhausted);
  EXPECT_TRUE(FailAllocation::Failed());
}

TEST(Allocation, InvalidOptionsDoNotAllocateAnExecutorOrThrowInternally) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 0;
  talon::StatusCode code = talon::StatusCode::kOk;
  {
    // The pinned status implementation allocates its diagnostic once. There
    // must be no executor allocation before validating the invalid options.
    FailAllocation fail(1);
    code = ConcurrentTaskLimiter::Create(options).status().code();
  }
  EXPECT_EQ(code, talon::StatusCode::kInvalidArgument);
  EXPECT_FALSE(FailAllocation::Failed());
}

TEST(Allocation, EveryFutureFailureDestroysCapturesEvenWithRetainedFuture) {
  ConcurrentTaskLimiter limiter(1);
  int failures = 0;
  bool succeeded = false;
  for (int allocation = 0; allocation < 16; ++allocation) {
    std::weak_ptr<int> weak;
    std::future<int> future;
    bool caught = false;
    {
      auto owner = std::make_shared<int>(42);
      weak = owner;
      auto callable = [owner = std::move(owner)] { return *owner; };
      FailAllocation fail(allocation);
      try {
        auto result = limiter.Submit(std::move(callable));
        if (result.ok()) {
          future = std::move(*result);
        }
      } catch (const std::bad_alloc&) {
        caught = true;
      }
    }
    EXPECT_TRUE(limiter.WaitIdle().ok());
    EXPECT_TRUE(weak.expired()) << "allocation=" << allocation;
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
    if (caught) {
      ++failures;
    } else {
      ASSERT_TRUE(future.valid());
      EXPECT_EQ(future.get(), 42);
      succeeded = true;
      break;
    }
  }
  EXPECT_GE(failures, 2);
  EXPECT_TRUE(succeeded);
}

TEST(Allocation, OverAlignedTaskAllocationFailurePreservesCallerOwnership) {
  class alignas(512) Callable {
   public:
    explicit Callable(std::unique_ptr<int> value) : value_(std::move(value)) {}
    Callable(Callable&&) = default;
    void operator()() {}
    bool has_value() const { return value_ != nullptr; }

   private:
    std::unique_ptr<int> value_;
  } callable(std::make_unique<int>(42));
  ConcurrentTaskLimiter limiter(1);
  bool caught = false;
  {
    FailAllocation fail(0);
    try {
      static_cast<void>(limiter.Schedule(std::move(callable)));
    } catch (const std::bad_alloc&) {
      caught = true;
    }
  }
  EXPECT_TRUE(caught);
  EXPECT_TRUE(callable.has_value());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

TEST(Allocation, RollbackDoesNotReleaseAnotherRunningTasksReservation) {
  talon_test::Gate release;
  ConcurrentTaskLimiter limiter(2);
  talon_test::OpenOnExit cleanup(release);
  ASSERT_TRUE(limiter.Schedule([&] { release.Wait(); }).ok());
  bool caught = false;
  {
    FailAllocation fail(0);
    try {
      static_cast<void>(limiter.Submit([] { return 42; }));
    } catch (const std::bad_alloc&) {
      caught = true;
    }
  }
  EXPECT_TRUE(caught);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
  EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
            talon::StatusCode::kDeadlineExceeded);
  release.Open();
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Allocation, RejectionDiagnosticFailureDoesNotMoveArgumentsOrChangeCount) {
  talon_test::Gate release;
  ConcurrentTaskLimiter limiter(1);
  talon_test::OpenOnExit cleanup(release);
  ASSERT_TRUE(limiter.Schedule([&] { release.Wait(); }).ok());
  auto argument = std::make_unique<int>(42);
  talon::StatusCode code = talon::StatusCode::kOk;
  bool caught = false;
  {
    FailAllocation fail(0);
    try {
      code = limiter.Schedule([](std::unique_ptr<int>) {}, std::move(argument))
                 .code();
    } catch (const std::bad_alloc&) {
      caught = true;
    }
  }
  EXPECT_TRUE(caught || code == talon::StatusCode::kResourceExhausted);
  EXPECT_NE(argument, nullptr);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
}

TEST(Allocation, PersistentFailureWhileConstructingErrorLeavesNoExecutor) {
  bool caught = false;
  {
    FailAllocation fail(0, true);
    try {
      static_cast<void>(
          ConcurrentTaskLimiter::Create(ConcurrentTaskLimiter::Options()));
    } catch (const std::bad_alloc&) {
      caught = true;
    }
  }
  EXPECT_TRUE(caught);
  auto recovered =
      ConcurrentTaskLimiter::Create(ConcurrentTaskLimiter::Options());
  ASSERT_TRUE(recovered.ok());
  EXPECT_TRUE((*recovered)->Shutdown().ok());
}
}  // namespace
