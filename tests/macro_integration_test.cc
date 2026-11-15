// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "talon/status_macros.h"
#include "test_support.h"

namespace {

talon::Status Propagate(talon::Status status, int* evaluated, int* continued) {
  auto once = [&] {
    ++*evaluated;
    return std::move(status);
  };
  TALON_RETURN_IF_ERROR(once());
  ++*continued;
  return talon::OkStatus();
}

TEST(MacroIntegration,
     ReturnPropagationPreservesBinaryMessageAndEvaluatesOnce) {
  const std::string message("failed\0stage", 12);
  int evaluated = 0;
  int continued = 0;
  auto result = Propagate(talon::Status(talon::StatusCode::kDataLoss, message),
                          &evaluated, &continued);
  EXPECT_EQ(evaluated, 1);
  EXPECT_EQ(continued, 0);
  EXPECT_EQ(result.code(), talon::StatusCode::kDataLoss);
  EXPECT_EQ(result.message(), message);
  EXPECT_TRUE(Propagate(talon::OkStatus(), &evaluated, &continued).ok());
  EXPECT_EQ(evaluated, 2);
  EXPECT_EQ(continued, 1);
}

talon::StatusOr<std::unique_ptr<int>> ThroughFuture(
    talon::ConcurrentTaskLimiter* limiter) {
  TALON_ASSIGN_OR_RETURN(
      auto future, limiter->Submit([] { return std::make_unique<int>(42); }));
  auto result = future.get();
  TALON_RETURN_IF_ERROR(limiter->WaitIdle());
  return result;
}

TEST(MacroIntegration, AssignMovesFutureAndMoveOnlyResultThroughStatusOr) {
  talon::ConcurrentTaskLimiter limiter(1);
  auto result = ThroughFuture(&limiter);
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_NE(*result, nullptr);
  EXPECT_EQ(**result, 42);
}

TEST(MacroIntegration, FailedAdmissionDoesNotReleaseAnExistingReservation) {
  talon_test::Gate release;
  talon::ConcurrentTaskLimiter limiter(1);
  talon_test::OpenOnExit cleanup(release);
  ASSERT_TRUE(limiter.Schedule([&] { release.Wait(); }).ok());
  for (int attempt = 0; attempt < 1000; ++attempt) {
    auto result = ThroughFuture(&limiter);
    ASSERT_EQ(result.status().code(), talon::StatusCode::kResourceExhausted);
    EXPECT_EQ(result.status().message(),
              "Maximum outstanding task limit (1) reached");
  }
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 1U);
  limiter.Close();
  EXPECT_EQ(ThroughFuture(&limiter).status().code(),
            talon::StatusCode::kFailedPrecondition);
}

class Cleanup {
 public:
  explicit Cleanup(std::atomic<int>* destroyed) : destroyed_(destroyed) {}
  ~Cleanup() { destroyed_->fetch_add(1); }

 private:
  std::atomic<int>* destroyed_;
};

talon::StatusOr<int> UserStages(std::atomic<int>* destroyed,
                                std::atomic<int>* after_error) {
  auto create = [destroyed]() -> talon::StatusOr<std::unique_ptr<Cleanup>> {
    return std::make_unique<Cleanup>(destroyed);
  };
  TALON_ASSIGN_OR_RETURN(auto resource, create());
  TALON_RETURN_IF_ERROR(talon::AbortedError("stage stopped"));
  after_error->fetch_add(1);
  return 42;
}

TEST(MacroIntegration, EarlyReturnRunsUserRaiiAndReleasesExecutorCapacity) {
  std::atomic<int> destroyed{0};
  std::atomic<int> after_error{0};
  talon::ConcurrentTaskLimiter limiter(1);
  for (int round = 0; round < 128; ++round) {
    auto result =
        limiter.Submit([&] { return UserStages(&destroyed, &after_error); });
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->get().status().code(), talon::StatusCode::kAborted);
    EXPECT_TRUE(limiter.WaitIdle().ok());
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  }
  EXPECT_EQ(destroyed.load(), 128);
  EXPECT_EQ(after_error.load(), 0);
}

TEST(MacroIntegration, FactoryPropagatesValidationBeforeWorkerCreation) {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 0;
  auto result = talon::ConcurrentTaskLimiter::Create(options);
  EXPECT_EQ(result.status().code(), talon::StatusCode::kInvalidArgument);
  EXPECT_EQ(result.status().message(), "max_concurrent must be positive");
}

}  // namespace
