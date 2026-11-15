// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "test_support.h"

namespace {
using talon::ConcurrentTaskLimiter;
using talon::StatusCode;
using talon_test::Gate;
using talon_test::OpenOnExit;

static_assert(!std::is_copy_constructible_v<ConcurrentTaskLimiter>);
static_assert(!std::is_move_constructible_v<ConcurrentTaskLimiter>);
static_assert(!std::is_copy_assignable_v<ConcurrentTaskLimiter>);
static_assert(!std::is_move_assignable_v<ConcurrentTaskLimiter>);
static_assert(std::is_nothrow_destructible_v<ConcurrentTaskLimiter>);

TEST(Validation, RejectsNullFunctionPointerBeforeDispatch) {
  ConcurrentTaskLimiter limiter(1);
  void (*function)() = nullptr;
  EXPECT_EQ(limiter.Schedule(function).code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

TEST(Validation, RejectsNullMemberFunctionPointerBeforeDispatch) {
  struct Object {
    void Run() {}
  } object;
  void (Object::*function)() = nullptr;
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(function, &object);
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Validation, RejectsNullMemberDataPointerBeforeDispatch) {
  struct Object {
    int value = 42;
  } object;
  int Object::*member = nullptr;
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(member, std::ref(object));
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Configuration, RejectsNonPositiveLimits) {
  for (int32_t limit : {0, -1, std::numeric_limits<int32_t>::min()}) {
    EXPECT_THROW(ConcurrentTaskLimiter limiter(limit), std::invalid_argument);
    ConcurrentTaskLimiter::Options options;
    options.max_concurrent = limit;
    auto result = ConcurrentTaskLimiter::Create(options);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  }
}

TEST(Configuration, RejectsInvalidWorkerCounts) {
  for (int32_t workers : {-1, 3, std::numeric_limits<int32_t>::min()}) {
    ConcurrentTaskLimiter::Options options;
    options.max_concurrent = 2;
    options.worker_count = workers;
    EXPECT_THROW(ConcurrentTaskLimiter limiter(options), std::invalid_argument);
    EXPECT_EQ(ConcurrentTaskLimiter::Create(options).status().code(),
              StatusCode::kInvalidArgument);
  }
}

TEST(Configuration, DefaultWorkersMatchLimit) {
  ConcurrentTaskLimiter limiter(3);
  const auto snapshot = limiter.GetSnapshot();
  EXPECT_EQ(snapshot.max_concurrent, 3);
  EXPECT_EQ(snapshot.worker_count, 3);
  EXPECT_TRUE(snapshot.accepting);
  EXPECT_EQ(snapshot.outstanding, 0U);
  EXPECT_EQ(snapshot.running, 0U);
  EXPECT_EQ(snapshot.queued, 0U);
}

TEST(Configuration, FactoryAndIndependentWorkerCount) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 8;
  options.worker_count = 2;
  auto result = ConcurrentTaskLimiter::Create(options);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ((*result)->GetSnapshot().worker_count, 2);
  EXPECT_TRUE((*result)->Shutdown().ok());
}

TEST(Configuration, LargeCapacityDoesNotPreallocateQueueOrThreads) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = std::numeric_limits<int32_t>::max();
  options.worker_count = 1;
  ConcurrentTaskLimiter limiter(options);
  auto result = limiter.Submit([] { return 42; });
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
  EXPECT_EQ(limiter.GetSnapshot().worker_count, 1);
  EXPECT_TRUE(limiter.Shutdown().ok());
}

TEST(Schedule, ExecutesAndReusesSingleSlot) {
  ConcurrentTaskLimiter limiter(1);
  int value = 0;
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(limiter.Schedule([&] { ++value; }).ok());
    ASSERT_TRUE(limiter.WaitIdle().ok());
    EXPECT_EQ(value, i + 1);
  }
}

TEST(Schedule, DiscardsReturnValueAndDestroysIt) {
  ConcurrentTaskLimiter limiter(1);
  auto token = std::make_shared<int>(12);
  std::weak_ptr<int> weak = token;
  ASSERT_TRUE(
      limiter.Schedule([token = std::move(token)] { return token; }).ok());
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_TRUE(weak.expired());
}

class MoveOnlyCallable {
 public:
  explicit MoveOnlyCallable(std::unique_ptr<int> value)
      : value_(std::move(value)) {}
  MoveOnlyCallable(MoveOnlyCallable&&) = default;
  MoveOnlyCallable(const MoveOnlyCallable&) = delete;
  int operator()(std::unique_ptr<int> other) && { return *value_ + *other; }
  bool has_value() const { return value_ != nullptr; }

 private:
  std::unique_ptr<int> value_;
};

TEST(Submission, MoveOnlyRvalueCallableAndArgument) {
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(MoveOnlyCallable(std::make_unique<int>(20)),
                               std::make_unique<int>(22));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
}

TEST(Schedule, MoveOnlyCaptureAndArgument) {
  ConcurrentTaskLimiter limiter(1);
  int result = 0;
  ASSERT_TRUE(
      limiter
          .Schedule(
              [value = std::make_unique<int>(20), &result](
                  std::unique_ptr<int> other) { result = *value + *other; },
              std::make_unique<int>(22))
          .ok());
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(result, 42);
}

TEST(Submission, MutableLambdaAndDecayedCopy) {
  ConcurrentTaskLimiter limiter(1);
  int original = 10;
  auto callable = [calls = 0](int value) mutable { return value + ++calls; };
  auto result = limiter.Submit(callable, original);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 11);
  EXPECT_EQ(callable(original), 11);
}

TEST(Submission, ExplicitReferenceAndLvalueOnlyCallable) {
  class LvalueOnly {
   public:
    void operator()(int& value) & { ++value; }
  } callable;
  ConcurrentTaskLimiter limiter(1);
  int value = 7;
  auto result = limiter.Submit(std::ref(callable), std::ref(value));
  ASSERT_TRUE(result.ok());
  result->get();
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(value, 8);
}

TEST(Submission, FunctionPointerAndStringLiteralDecay) {
  ConcurrentTaskLimiter limiter(1);
  auto length = +[](const char* text) { return std::string(text).size(); };
  auto result = limiter.Submit(length, "hello");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 5U);
}

TEST(Submission, MemberFunctionAndMemberData) {
  struct Object {
    int Add(int increment) const { return value + increment; }
    int value = 20;
  } object;
  ConcurrentTaskLimiter limiter(2);
  auto method = limiter.Submit(&Object::Add, &object, 22);
  auto field = limiter.Submit(&Object::value, std::ref(object));
  ASSERT_TRUE(method.ok());
  ASSERT_TRUE(field.ok());
  EXPECT_EQ(method->get(), 42);
  EXPECT_EQ(&field->get(), &object.value);
}

TEST(Submission, MoveOnlyResult) {
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit([] { return std::make_unique<int>(42); });
  ASSERT_TRUE(result.ok());
  auto value = result->get();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 42);
}

TEST(Submission, ReferenceAndConstReferenceResults) {
  ConcurrentTaskLimiter limiter(2);
  int value = 42;
  auto ref = limiter.Submit([&]() -> int& { return value; });
  auto cref = limiter.Submit([&]() -> const int& { return value; });
  ASSERT_TRUE(ref.ok());
  ASSERT_TRUE(cref.ok());
  EXPECT_EQ(&ref->get(), &value);
  EXPECT_EQ(&cref->get(), &value);
}

TEST(Submission, VoidAndAbandonedFuture) {
  ConcurrentTaskLimiter limiter(2);
  std::atomic<int> calls{0};
  {
    auto abandoned = limiter.Submit([&] { ++calls; });
    ASSERT_TRUE(abandoned.ok());
  }
  auto result = limiter.Submit([&] { ++calls; });
  ASSERT_TRUE(result.ok());
  EXPECT_NO_THROW(result->get());
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(calls.load(), 2);
}

TEST(Submission, CapturesReleasedEvenWhenFutureIsRetained) {
  ConcurrentTaskLimiter limiter(1);
  auto token = std::make_shared<int>(42);
  std::weak_ptr<int> weak = token;
  auto result = limiter.Submit([token = std::move(token)] { return *token; });
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(result->get(), 42);
}

TEST(Submission, FutureOutlivesExecutor) {
  std::future<std::string> future;
  {
    ConcurrentTaskLimiter limiter(1);
    auto result = limiter.Submit([] { return std::string("done"); });
    ASSERT_TRUE(result.ok());
    future = std::move(*result);
  }
  EXPECT_EQ(future.get(), "done");
}

TEST(Submission, StandardAndNonStandardExceptionsReachFuture) {
  ConcurrentTaskLimiter limiter(1);
  for (int kind = 0; kind < 3; ++kind) {
    auto result = limiter.Submit([kind]() -> int {
      if (kind == 0) {
        throw std::runtime_error("task failure");
      }
      if (kind == 1) {
        throw 17;
      }
      throw std::bad_alloc();
    });
    ASSERT_TRUE(result.ok());
    if (kind == 0) {
      EXPECT_THROW(result->get(), std::runtime_error);
    }
    if (kind == 1) {
      EXPECT_THROW(result->get(), int);
    }
    if (kind == 2) {
      EXPECT_THROW(result->get(), std::bad_alloc);
    }
    ASSERT_TRUE(limiter.WaitIdle().ok());
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  }
}

TEST(Submission, EmptyFunctionReportsBadFunctionCall) {
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(std::function<void()>{});
  ASSERT_TRUE(result.ok());
  EXPECT_THROW(result->get(), std::bad_function_call);
}

TEST(Submission, ThrowingResultMoveReachesFuture) {
  class Result {
   public:
    // A non-default constructor makes every supported standard library use
    // move construction. MSVC's promise otherwise uses move assignment.
    explicit Result(std::atomic<int>* moves) : moves_(moves) {}
    Result(const Result&) = delete;
    Result(Result&& other) : moves_(other.moves_) {
      moves_->fetch_add(1);
      throw std::runtime_error("result move");
    }

   private:
    std::atomic<int>* moves_;
  };
  std::atomic<int> moves{0};
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit([&moves] { return Result(&moves); });
  ASSERT_TRUE(result.ok());
  result->wait();
  EXPECT_EQ(moves.load(), 1);
  EXPECT_THROW(result->get(), std::runtime_error);
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Admission, FullDoesNotMoveCallableOrArguments) {
  Gate gate;
  ConcurrentTaskLimiter limiter(1);
  OpenOnExit release(gate);
  ASSERT_TRUE(limiter.Schedule([&] { gate.Wait(); }).ok());
  MoveOnlyCallable callable(std::make_unique<int>(20));
  auto argument = std::make_unique<int>(22);
  auto result = limiter.Submit(std::move(callable), std::move(argument));
  EXPECT_EQ(result.status().code(), StatusCode::kResourceExhausted);
  EXPECT_TRUE(callable.has_value());
  EXPECT_NE(argument, nullptr);
  auto status = limiter.Schedule(std::move(callable), std::move(argument));
  EXPECT_EQ(status.code(), StatusCode::kResourceExhausted);
  EXPECT_TRUE(callable.has_value());
  EXPECT_NE(argument, nullptr);
}

TEST(Admission, ClosedTakesPrecedenceOverFullAndPreservesArguments) {
  Gate gate;
  ConcurrentTaskLimiter limiter(1);
  OpenOnExit release(gate);
  ASSERT_TRUE(limiter.Schedule([&] { gate.Wait(); }).ok());
  limiter.Close();
  auto value = std::make_unique<int>(42);
  auto result = limiter.Submit([](std::unique_ptr<int>) {}, std::move(value));
  EXPECT_EQ(result.status().code(), StatusCode::kFailedPrecondition);
  EXPECT_NE(value, nullptr);
  EXPECT_EQ(limiter.Schedule([] {}).code(), StatusCode::kFailedPrecondition);
}

class ThrowOnCapture {
 public:
  explicit ThrowOnCapture(bool move_fails = false) : move_fails_(move_fails) {}
  ThrowOnCapture(const ThrowOnCapture&) { throw std::runtime_error("copy"); }
  ThrowOnCapture(ThrowOnCapture&& other) : move_fails_(other.move_fails_) {
    if (move_fails_) {
      throw std::runtime_error("move");
    }
  }
  void operator()() {}

 private:
  bool move_fails_;
};

TEST(Admission, ThrowingCallableCopyAndMoveRollBack) {
  ConcurrentTaskLimiter limiter(1);
  ThrowOnCapture copy;
  EXPECT_THROW(static_cast<void>(limiter.Schedule(copy)), std::runtime_error);
  EXPECT_THROW(static_cast<void>(limiter.Submit(copy)), std::runtime_error);
  ThrowOnCapture move(true);
  EXPECT_THROW(static_cast<void>(limiter.Schedule(std::move(move))),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(limiter.Submit(std::move(move))),
               std::runtime_error);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  EXPECT_TRUE(limiter.Schedule([] {}).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Admission, ThrowingArgumentCopyAndMoveRollBack) {
  ConcurrentTaskLimiter limiter(1);
  ThrowOnCapture copy;
  auto function = [](ThrowOnCapture) {};
  EXPECT_THROW(static_cast<void>(limiter.Schedule(function, copy)),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(limiter.Submit(function, copy)),
               std::runtime_error);
  ThrowOnCapture move(true);
  EXPECT_THROW(static_cast<void>(limiter.Schedule(function, std::move(move))),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(limiter.Submit(function, std::move(move))),
               std::runtime_error);
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
  EXPECT_TRUE(limiter.Schedule([] {}).ok());
  EXPECT_TRUE(limiter.WaitIdle().ok());
}

TEST(Admission, EarlierCapturesDestroyedWhenLaterConstructionThrows) {
  ConcurrentTaskLimiter limiter(1);
  auto token = std::make_shared<int>(42);
  std::weak_ptr<int> weak = token;
  ThrowOnCapture argument;
  EXPECT_THROW(static_cast<void>(limiter.Schedule(
                   [token = std::move(token)](ThrowOnCapture) {}, argument)),
               std::runtime_error);
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
}

TEST(Submission, OverAlignedCallableAndArgument) {
  struct alignas(256) Argument {
    int value = 42;
  };
  class alignas(256) Callable {
   public:
    int operator()(Argument&& argument) && {
      EXPECT_EQ(reinterpret_cast<std::uintptr_t>(this) % alignof(Callable), 0U);
      EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&argument) % alignof(Argument),
                0U);
      return argument.value;
    }
  };
  ConcurrentTaskLimiter limiter(1);
  auto result = limiter.Submit(Callable{}, Argument{});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->get(), 42);
}
}  // namespace
