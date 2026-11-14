// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
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
using namespace std::chrono_literals;

TEST(Lifecycle, EmptyWaitAndRepeatedShutdown) {
  ConcurrentTaskLimiter limiter(2);
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_TRUE(
      limiter.WaitIdleUntil(std::chrono::steady_clock::time_point::min()).ok());
  for (int i = 0; i < 4; ++i) {
    limiter.Close();
    EXPECT_TRUE(limiter.Shutdown().ok());
    EXPECT_TRUE(limiter.WaitIdle().ok());
  }
  EXPECT_FALSE(limiter.GetSnapshot().accepting);
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kFailedPrecondition);
}

TEST(Lifecycle, DeadlineDoesNotCancelTasks) {
  Gate gate;
  ConcurrentTaskLimiter limiter(1);
  OpenOnExit release(gate);
  auto result = limiter.Submit([&] {
    gate.Wait();
    return 42;
  });
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
            StatusCode::kDeadlineExceeded);
  EXPECT_TRUE(limiter.GetSnapshot().accepting);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
  gate.Open();
  EXPECT_TRUE(
      limiter.WaitIdleUntil(std::chrono::steady_clock::now() + 20s).ok());
  EXPECT_EQ(result->get(), 42);
}

TEST(Lifecycle, QueuedTasksConsumeCapacityAndDrainAfterClose) {
  Gate release;
  Countdown started(1);
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 5;
  options.worker_count = 1;
  ConcurrentTaskLimiter limiter(options);
  OpenOnExit cleanup(release);
  ASSERT_TRUE(limiter
                  .Schedule([&] {
                    started.Arrive();
                    release.Wait();
                  })
                  .ok());
  started.Wait();
  std::vector<int> order;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(limiter.Schedule([&, i] { order.push_back(i); }).ok());
  }
  const auto snapshot = limiter.GetSnapshot();
  EXPECT_EQ(snapshot.outstanding, 5U);
  EXPECT_EQ(snapshot.running, 1U);
  EXPECT_EQ(snapshot.queued, 4U);
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kResourceExhausted);
  limiter.Close();
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kFailedPrecondition);
  release.Open();
  EXPECT_TRUE(limiter.Shutdown().ok());
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
}

TEST(Lifecycle, WorkerCannotWaitForItselfAndErrorsDoNotCloseExecutor) {
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit([&] {
    EXPECT_EQ(limiter.WaitIdle().code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
              StatusCode::kFailedPrecondition);
    EXPECT_EQ(limiter.Shutdown().code(), StatusCode::kFailedPrecondition);
    EXPECT_TRUE(limiter.GetSnapshot().accepting);
  });
  ASSERT_TRUE(result.ok());
  result->get();
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_TRUE(limiter.Schedule([] {}).ok());
}

TEST(Lifecycle, WorkerSelfShutdownDoesNotBlockBehindExternalJoiner) {
  Gate proceed;
  Countdown started(1);
  ConcurrentTaskLimiter limiter(1);
  OpenOnExit cleanup(proceed);
  auto task = limiter.Submit([&] {
    started.Arrive();
    proceed.Wait();
    return limiter.Shutdown().code();
  });
  ASSERT_TRUE(task.ok());
  started.Wait();
  auto shutdown =
      std::async(std::launch::async, [&] { return limiter.Shutdown(); });
  talon_test::SpinUntil([&] { return !limiter.GetSnapshot().accepting; });
  proceed.Open();
  EXPECT_EQ(task->get(), StatusCode::kFailedPrecondition);
  EXPECT_TRUE(shutdown.get().ok());
}

TEST(Lifecycle, WorkerCanCloseAndAlreadyQueuedWorkStillRuns) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 3;
  options.worker_count = 1;
  Gate proceed;
  ConcurrentTaskLimiter limiter(options);
  OpenOnExit cleanup(proceed);
  auto first = limiter.Submit([&] {
    proceed.Wait();
    limiter.Close();
  });
  auto second = limiter.Submit([] { return 42; });
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  proceed.Open();
  first->get();
  EXPECT_EQ(second->get(), 42);
  EXPECT_TRUE(limiter.Shutdown().ok());
}

TEST(Lifecycle, NestedScheduleSucceedsWithSpareCapacity) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 2;
  options.worker_count = 1;
  ConcurrentTaskLimiter limiter(options);
  std::atomic<int> calls{0};
  ASSERT_TRUE(limiter
                  .Schedule([&] {
                    ++calls;
                    EXPECT_TRUE(limiter.Schedule([&] { ++calls; }).ok());
                  })
                  .ok());
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(calls.load(), 2);
}

TEST(Lifecycle, NestedScheduleRejectsWithoutDeadlockingWhenFull) {
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit([&] { return limiter.Schedule([] {}).code(); });
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), StatusCode::kResourceExhausted);
}

TEST(Lifecycle, WaitingOnDifferentExecutorIsAllowed) {
  ConcurrentTaskLimiter outer(1);
  ConcurrentTaskLimiter inner(1);
  auto result = outer.Submit([&] {
    EXPECT_TRUE(inner.Schedule([] {}).ok());
    EXPECT_TRUE(inner.WaitIdle().ok());
    EXPECT_TRUE(inner.Shutdown().ok());
  });
  ASSERT_TRUE(result.ok());
  result->get();
}

struct CopyCallback {
  explicit CopyCallback(std::function<void()> callback)
      : callback(std::move(callback)) {}
  CopyCallback(const CopyCallback& other) : callback(other.callback) {
    callback();
  }
  CopyCallback(CopyCallback&&) = default;
  void operator()() {}
  std::function<void()> callback;
};

TEST(Lifecycle, CaptureConstructorCanInspectAndCannotSelfWait) {
  ConcurrentTaskLimiter limiter(1);
  CopyCallback callable([&] {
    const auto snapshot = limiter.GetSnapshot();
    EXPECT_EQ(snapshot.outstanding, 1U);
    EXPECT_EQ(snapshot.queued, 0U);
    EXPECT_EQ(snapshot.running, 0U);
    EXPECT_EQ(limiter.WaitIdle().code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
              StatusCode::kFailedPrecondition);
    EXPECT_EQ(limiter.Shutdown().code(), StatusCode::kFailedPrecondition);
    EXPECT_TRUE(limiter.GetSnapshot().accepting);
    EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kResourceExhausted);
  });
  EXPECT_TRUE(limiter.Schedule(callable).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
  auto result = limiter.Submit(callable);
  ASSERT_TRUE(result.ok());
  result->get();
}

TEST(Lifecycle, NestedConstructionTracksEveryReservationInThread) {
  ConcurrentTaskLimiter outer(2);
  ConcurrentTaskLimiter inner(1);
  CopyCallback inner_callable([&] {
    EXPECT_EQ(outer.WaitIdle().code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(inner.WaitIdle().code(), StatusCode::kFailedPrecondition);
  });
  CopyCallback outer_callable([&] {
    EXPECT_TRUE(inner.Schedule(inner_callable).ok());
    EXPECT_EQ(outer.Shutdown().code(), StatusCode::kFailedPrecondition);
  });
  EXPECT_TRUE(outer.Schedule(outer_callable).ok());
  EXPECT_TRUE(outer.WaitIdle().ok());
  EXPECT_TRUE(inner.WaitIdle().ok());
}

TEST(Lifecycle, CloseDuringConstructionStillPublishesAcceptedTask) {
  Gate entered;
  Gate proceed;
  ConcurrentTaskLimiter limiter(1);
  CopyCallback callable([&] {
    entered.Open();
    proceed.Wait();
  });
  auto submission = std::async(std::launch::async,
                               [&] { return limiter.Schedule(callable); });
  OpenOnExit cleanup(proceed);
  entered.Wait();
  limiter.Close();
  EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
            StatusCode::kDeadlineExceeded);
  auto shutdown =
      std::async(std::launch::async, [&] { return limiter.Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(0s), std::future_status::timeout);
  proceed.Open();
  EXPECT_TRUE(submission.get().ok());
  EXPECT_TRUE(shutdown.get().ok());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

TEST(Lifecycle, CloseDuringFailingConstructionWakesIdleAndShutdown) {
  Gate entered;
  Gate proceed;
  ConcurrentTaskLimiter limiter(1);
  CopyCallback callable([&] {
    entered.Open();
    proceed.Wait();
    throw std::runtime_error("capture failure");
  });
  auto submission = std::async(std::launch::async,
                               [&] { return limiter.Schedule(callable); });
  OpenOnExit cleanup(proceed);
  entered.Wait();
  limiter.Close();
  auto waiter =
      std::async(std::launch::async, [&] { return limiter.WaitIdle(); });
  auto shutdown =
      std::async(std::launch::async, [&] { return limiter.Shutdown(); });
  proceed.Open();
  EXPECT_THROW(submission.get(), std::runtime_error);
  EXPECT_TRUE(waiter.get().ok());
  EXPECT_TRUE(shutdown.get().ok());
}

TEST(Lifecycle, CaptureCanCloseThenThrowAndReservationRollsBack) {
  ConcurrentTaskLimiter limiter(1);
  CopyCallback callable([&] {
    limiter.Close();
    throw std::runtime_error("capture failure");
  });
  EXPECT_THROW(static_cast<void>(limiter.Schedule(callable)),
               std::runtime_error);
  EXPECT_TRUE(limiter.Shutdown().ok());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

struct DestructorCallback {
  explicit DestructorCallback(std::function<void()> callback)
      : callback(std::move(callback)) {}
  ~DestructorCallback() { callback(); }
  std::function<void()> callback;
};

TEST(Lifecycle, CaptureDestructorRunsUnlockedAndRetainsItsSlot) {
  ConcurrentTaskLimiter limiter(1);
  auto capture = std::make_unique<DestructorCallback>([&] {
    EXPECT_EQ(limiter.GetSnapshot().running, 1U);
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
    EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kResourceExhausted);
    EXPECT_EQ(limiter.WaitIdle().code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(limiter.Shutdown().code(), StatusCode::kFailedPrecondition);
    limiter.Close();
  });
  EXPECT_TRUE(limiter.Schedule([capture = std::move(capture)] {}).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_FALSE(limiter.GetSnapshot().accepting);
}

TEST(Lifecycle, ReadyFutureDoesNotReleaseSlotBeforeCaptureDestructor) {
  Gate destructor_entered;
  Gate release_destructor;
  ConcurrentTaskLimiter limiter(1);
  OpenOnExit cleanup(release_destructor);
  auto capture = std::make_unique<DestructorCallback>([&] {
    destructor_entered.Open();
    release_destructor.Wait();
  });
  auto result = limiter.Submit([capture = std::move(capture)] { return 42; });
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
  destructor_entered.Wait();
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kResourceExhausted);
  EXPECT_EQ(limiter.WaitIdleUntil(std::chrono::steady_clock::now()).code(),
            StatusCode::kDeadlineExceeded);
  release_destructor.Open();
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Lifecycle, CaptureDestructorCanSubmitMoreWorkAndIdleIncludesIt) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 2;
  options.worker_count = 1;
  ConcurrentTaskLimiter limiter(options);
  int executions = 0;
  auto capture = std::make_unique<DestructorCallback>(
      [&] { EXPECT_TRUE(limiter.Schedule([&] { ++executions; }).ok()); });
  ASSERT_TRUE(limiter.Schedule([capture = std::move(capture)] {}).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(executions, 1);
}

TEST(Lifecycle, FailedConstructionDestroysEarlierCaptureOutsideLock) {
  ConcurrentTaskLimiter limiter(1);
  int destroyed = 0;
  auto capture = std::make_unique<DestructorCallback>([&] {
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
    EXPECT_EQ(limiter.WaitIdle().code(), StatusCode::kFailedPrecondition);
    ++destroyed;
  });
  struct FailingArgument {
    FailingArgument() = default;
    FailingArgument(const FailingArgument&) { throw 17; }
  } argument;
  EXPECT_THROW(
      static_cast<void>(limiter.Schedule(
          [capture = std::move(capture)](const FailingArgument&) {}, argument)),
      int);
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Lifecycle, ArgumentDestructorAlsoRetainsSlotAndCanReenter) {
  ConcurrentTaskLimiter limiter(1);
  int destroyed = 0;
  auto capture = std::make_unique<DestructorCallback>([&] {
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
    EXPECT_EQ(limiter.GetSnapshot().running, 1U);
    EXPECT_EQ(limiter.WaitIdle().code(), StatusCode::kFailedPrecondition);
    ++destroyed;
  });
  auto result = limiter.Submit(
      [](const std::unique_ptr<DestructorCallback>&) {}, std::move(capture));
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(destroyed, 1);
  result->get();
}

TEST(Lifecycle, DestructorDrainsAllAcceptedTasks) {
  Gate proceed;
  Gate destroying;
  std::atomic<int> calls{0};
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 32;
  options.worker_count = 4;
  auto limiter = std::make_unique<ConcurrentTaskLimiter>(options);
  for (int i = 0; i < 32; ++i) {
    ASSERT_TRUE(limiter
                    ->Schedule([&] {
                      proceed.Wait();
                      ++calls;
                    })
                    .ok());
  }
  auto destruction =
      std::async(std::launch::async, [&, owner = std::move(limiter)]() mutable {
        destroying.Open();
        owner.reset();
      });
  OpenOnExit cleanup(proceed);
  destroying.Wait();
  EXPECT_EQ(destruction.wait_for(0s), std::future_status::timeout);
  proceed.Open();
  destruction.get();
  EXPECT_EQ(calls.load(), 32);
}

TEST(Exceptions, HandlerRunsUnlockedAndMayClose) {
  std::atomic<int> errors{0};
  ConcurrentTaskLimiter* pointer = nullptr;
  ConcurrentTaskLimiter::Options options;
  options.on_unhandled_exception = [&](std::exception_ptr error) {
    EXPECT_THROW(std::rethrow_exception(error), std::logic_error);
    EXPECT_EQ(pointer->GetSnapshot().outstanding, 1U);
    EXPECT_EQ(pointer->WaitIdle().code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(pointer->Shutdown().code(), StatusCode::kFailedPrecondition);
    pointer->Close();
    ++errors;
  };
  ConcurrentTaskLimiter limiter(options);
  pointer = &limiter;
  EXPECT_TRUE(limiter.Schedule([] { throw std::logic_error("failure"); }).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(errors.load(), 1);
  EXPECT_FALSE(limiter.GetSnapshot().accepting);
}

TEST(Exceptions, SubmitDoesNotCallScheduleExceptionHandler) {
  std::atomic<int> errors{0};
  ConcurrentTaskLimiter::Options options;
  options.on_unhandled_exception = [&](std::exception_ptr) { ++errors; };
  ConcurrentTaskLimiter limiter(options);
  auto result = limiter.Submit([] { throw 99; });
  ASSERT_TRUE(result.ok());
  EXPECT_THROW(result->get(), int);
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(errors.load(), 0);
}

TEST(ExceptionsDeathTest, MissingHandlerTerminates) {
  EXPECT_EXIT(
      {
        std::set_terminate([] { std::_Exit(73); });
        ConcurrentTaskLimiter limiter(1);
        static_cast<void>(limiter.Schedule([] { throw 42; }));
        static_cast<void>(limiter.WaitIdle());
      },
      ::testing::ExitedWithCode(73), "");
}

TEST(ExceptionsDeathTest, ThrowingHandlerTerminates) {
  EXPECT_EXIT(
      {
        std::set_terminate([] { std::_Exit(73); });
        ConcurrentTaskLimiter::Options options;
        options.on_unhandled_exception = [](std::exception_ptr) { throw 17; };
        ConcurrentTaskLimiter limiter(options);
        static_cast<void>(limiter.Schedule([] { throw 42; }));
        static_cast<void>(limiter.WaitIdle());
      },
      ::testing::ExitedWithCode(73), "");
}

TEST(LifecycleDeathTest, DestructionOnOwnWorkerTerminates) {
  EXPECT_EXIT(
      {
        std::set_terminate([] { std::_Exit(73); });
        auto* limiter = new ConcurrentTaskLimiter(1);
        Gate proceed;
        static_cast<void>(limiter->Schedule([&] {
          proceed.Wait();
          delete limiter;
        }));
        proceed.Open();
        Gate never;
        never.Wait();
      },
      ::testing::ExitedWithCode(73), "");
}

TEST(LifecycleDeathTest, DestructionFromCaptureConstructorTerminates) {
  EXPECT_EXIT(
      {
        std::set_terminate([] { std::_Exit(73); });
        auto* limiter = new ConcurrentTaskLimiter(1);
        CopyCallback callable([&] { delete limiter; });
        static_cast<void>(limiter->Schedule(callable));
      },
      ::testing::ExitedWithCode(73), "");
}
}  // namespace
