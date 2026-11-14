// SPDX-License-Identifier: Apache-2.0
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "test_support.h"

namespace {

using talon::ConcurrentTaskLimiter;
using talon::StatusCode;

int Twice(int value) noexcept { return value * 2; }

class MemberTarget {
 public:
  int Read() const noexcept { return value_; }
  int ReadVolatile() volatile { return value_; }
  int Increment(int amount) { return value_ += amount; }

 private:
  int value_ = 40;
};

class DerivedTarget : public MemberTarget {};

struct DataTarget {
  int value = 40;
};

TEST(Invocation, NullRawMemberTargetIsRejected) {
  ConcurrentTaskLimiter limiter(1);
  MemberTarget* object = nullptr;
  auto result = limiter.Submit(&MemberTarget::Read, object);
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(limiter.Schedule(&MemberTarget::Increment, object, 2).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

TEST(Invocation, NullUniqueMemberTargetIsRejectedWithoutMoving) {
  ConcurrentTaskLimiter limiter(1);
  std::unique_ptr<MemberTarget> object;
  auto result = limiter.Submit(&MemberTarget::Read, std::move(object));
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(object, nullptr);
  EXPECT_TRUE(limiter.Schedule([] {}).ok());
}

TEST(Invocation, NullSharedAndAliasingMemberTargetsAreRejected) {
  ConcurrentTaskLimiter limiter(1);
  std::shared_ptr<MemberTarget> empty;
  EXPECT_EQ(limiter.Submit(&MemberTarget::Read, empty).status().code(),
            StatusCode::kInvalidArgument);
  auto owner = std::make_shared<int>(42);
  std::shared_ptr<MemberTarget> alias(owner, nullptr);
  EXPECT_EQ(limiter.Schedule(&MemberTarget::Read, std::move(alias)).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(alias.use_count(), 2);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Invocation, NullNoexceptFunctionPointerIsRejected) {
  ConcurrentTaskLimiter limiter(1);
  int (*function)(int) noexcept = nullptr;
  EXPECT_EQ(limiter.Submit(function, 42).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(limiter.Schedule(function, 42).code(),
            StatusCode::kInvalidArgument);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Invocation, ClosedAndFullAdmissionTakePrecedenceOverNullValidation) {
  talon_test::Gate gate;
  ConcurrentTaskLimiter limiter(1);
  talon_test::OpenOnExit cleanup(gate);
  ASSERT_TRUE(limiter.Schedule([&] { gate.Wait(); }).ok());
  void (*function)() = nullptr;
  EXPECT_EQ(limiter.Schedule(function).code(), StatusCode::kResourceExhausted);
  limiter.Close();
  EXPECT_EQ(limiter.Submit(function).status().code(),
            StatusCode::kFailedPrecondition);
}

TEST(Invocation, FunctionReferenceIsNotMistakenForNullableObject) {
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(Twice, 21);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
}

TEST(Invocation, NonNullUniqueMemberTargetTransfersOwnership) {
  ConcurrentTaskLimiter limiter(1);
  auto object = std::make_unique<MemberTarget>();
  auto result = limiter.Submit(&MemberTarget::Increment, std::move(object), 2);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(object, nullptr);
  EXPECT_EQ(result->get(), 42);
}

TEST(Invocation, SharedDerivedObjectSupportsBaseMemberPointer) {
  ConcurrentTaskLimiter limiter(1);
  auto object = std::make_shared<DerivedTarget>();
  auto result = limiter.Submit(&MemberTarget::Increment, object, 2);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(object.use_count(), 1);
}

TEST(Invocation, VolatileMemberFunctionAndObjectAreSupported) {
  ConcurrentTaskLimiter limiter(1);
  volatile MemberTarget object;
  auto result = limiter.Submit(&MemberTarget::ReadVolatile, &object);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 40);
}

TEST(Invocation, ConstMemberDataReferencePreservesIdentity) {
  ConcurrentTaskLimiter limiter(1);
  const DataTarget object;
  auto result = limiter.Submit(&DataTarget::value, std::cref(object));
  static_assert(std::is_same_v<decltype(result),
                               talon::StatusOr<std::future<const int&>>>);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(&result->get(), &object.value);
}

TEST(Invocation, ExplicitlyBorrowedCallableIsNotCopiedOrMoved) {
  class Callable {
   public:
    Callable() = default;
    Callable(const Callable&) = delete;
    Callable(Callable&&) = delete;
    int operator()() & { return ++calls_; }
    int calls() const { return calls_; }

   private:
    int calls_ = 0;
  } callable;
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(std::ref(callable));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 1);
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(callable.calls(), 1);
}

TEST(Invocation, ConstReferenceWrapperDoesNotDecayIntoCopy) {
  class NonCopyable {
   public:
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    int value() const { return 42; }
  } value;
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(
      [](const NonCopyable& argument) { return argument.value(); },
      std::cref(value));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
}

TEST(Invocation, HeterogeneousArgumentsPreserveOrderAndValueCategory) {
  ConcurrentTaskLimiter limiter(1);
  int borrowed = 4;
  auto result = limiter.Submit(
      [](std::unique_ptr<int>&& owned, std::string&& text, int& reference,
         const char* suffix, double factor) {
        reference += *owned;
        return text + suffix +
               std::to_string(reference * static_cast<int>(factor));
      },
      std::make_unique<int>(3), std::string("value"), std::ref(borrowed), "=",
      6.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), "value=42");
  EXPECT_EQ(borrowed, 7);
}

TEST(Invocation, ArrayBorrowIsExplicitAndRetainsExtent) {
  ConcurrentTaskLimiter limiter(1);
  int values[3] = {4, 5, 6};
  auto result = limiter.Submit(
      [](int(&array)[3]) {
        array[1] = 42;
        return sizeof(array);
      },
      std::ref(values));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), sizeof(values));
  EXPECT_EQ(values[1], 42);
}

TEST(Invocation, ImplicitArgumentConversionHappensOnWorker) {
  class Argument {
   public:
    explicit Argument(std::thread::id* thread) : thread_(thread) {}
    // Deliberately model an external callable's implicit argument conversion.
    operator int() const {  // NOLINT(google-explicit-constructor)
      *thread_ = std::this_thread::get_id();
      return 42;
    }

   private:
    std::thread::id* thread_;
  };
  ConcurrentTaskLimiter limiter(1);
  std::thread::id converted_on;
  const auto caller = std::this_thread::get_id();
  auto result =
      limiter.Submit([](int value) { return value; }, Argument(&converted_on));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
  EXPECT_NE(converted_on, caller);
}

TEST(Invocation, TemporaryArgumentConversionFailureReachesFuture) {
  class Argument {
   public:
    // The test requires an implicit conversion, including its failure path.
    operator int() const { throw 31; }  // NOLINT(google-explicit-constructor)
  };
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit([](int value) { return value; }, Argument());
  ASSERT_TRUE(result.ok());
  EXPECT_THROW(result->get(), int);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Invocation, StatusResultsCarryBusinessErrorsWithoutThrowing) {
  ConcurrentTaskLimiter limiter(2);
  auto status =
      limiter.Submit([] { return talon::InvalidArgumentError("input"); });
  auto value = limiter.Submit([]() -> talon::StatusOr<int> {
    return talon::ResourceExhaustedError("quota");
  });
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(status->get().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(value->get().status().code(), StatusCode::kResourceExhausted);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Invocation, SharedFutureSupportsManyReadersAndOutlivesPool) {
  std::shared_future<std::string> future;
  {
    ConcurrentTaskLimiter limiter(1);
    auto result = limiter.Submit([] { return std::string("published"); });
    ASSERT_TRUE(result.ok());
    future = result->share();
  }
  std::vector<std::thread> readers;
  for (int i = 0; i < 32; ++i) {
    readers.emplace_back([future] { EXPECT_EQ(future.get(), "published"); });
  }
  for (auto& reader : readers) {
    reader.join();
  }
}

TEST(Invocation, WorkerExceptionOutlivesExecutorAndFutureCanBeAbandoned) {
  std::future<int> future;
  {
    ConcurrentTaskLimiter limiter(2);
    auto retained = limiter.Submit([]() -> int { throw 42; });
    ASSERT_TRUE(retained.ok());
    future = std::move(*retained);
    auto discarded = limiter.Submit([]() -> int { throw 43; });
    ASSERT_TRUE(discarded.ok());
  }
  EXPECT_THROW(future.get(), int);
}

}  // namespace
