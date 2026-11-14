// SPDX-License-Identifier: Apache-2.0
// Linux-only ELF interposition, in a separate non-sanitized process.
#include <dlfcn.h>
#include <pthread.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"

namespace {
thread_local int starts_before_failure = -1;
thread_local bool start_failed = false;
thread_local bool measure_startup = false;
thread_local int successful_starts = 0;
thread_local int successful_joins = 0;
}  // namespace

extern "C" int pthread_create(pthread_t* thread,
                              const pthread_attr_t* attributes,
                              void* (*entry)(void*), void* argument) noexcept {
  if (starts_before_failure >= 0 && starts_before_failure-- == 0) {
    starts_before_failure = -1;
    start_failed = true;
    return EAGAIN;
  }
  using Create =
      int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
  static const auto real_create =
      reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
  if (!real_create) {
    std::abort();
  }
  const int result = real_create(thread, attributes, entry, argument);
  if (measure_startup && result == 0) {
    ++successful_starts;
  }
  return result;
}

extern "C" int pthread_join(pthread_t thread, void** result) {
  using Join = int (*)(pthread_t, void**);
  static const auto real_join =
      reinterpret_cast<Join>(dlsym(RTLD_NEXT, "pthread_join"));
  if (!real_join) {
    std::abort();
  }
  const int status = real_join(thread, result);
  if (measure_startup && status == 0) {
    ++successful_joins;
  }
  return status;
}

namespace {

class MeasureStartup {
 public:
  explicit MeasureStartup(int failure_index) {
    starts_before_failure = failure_index;
    start_failed = false;
    successful_starts = 0;
    successful_joins = 0;
    measure_startup = true;
  }
  ~MeasureStartup() {
    starts_before_failure = -1;
    measure_startup = false;
  }
  MeasureStartup(const MeasureStartup&) = delete;
  MeasureStartup& operator=(const MeasureStartup&) = delete;
};

struct StartupCase {
  int successful_before_failure;
  bool factory;
};

class PartialStartup : public ::testing::TestWithParam<StartupCase> {};

TEST_P(PartialStartup, EveryStartedThreadIsJoinedBeforeFailureReturns) {
  const StartupCase config = GetParam();
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = config.successful_before_failure + 1;
  bool reported = false;
  {
    MeasureStartup measure(config.successful_before_failure);
    if (config.factory) {
      auto result = talon::ConcurrentTaskLimiter::Create(options);
      reported =
          result.status().code() == talon::StatusCode::kResourceExhausted;
    } else {
      try {
        talon::ConcurrentTaskLimiter limiter(options);
      } catch (const std::system_error& error) {
        reported = error.code().value() == EAGAIN;
      }
    }
  }
  EXPECT_TRUE(reported);
  EXPECT_TRUE(start_failed);
  EXPECT_EQ(successful_starts, config.successful_before_failure);
  EXPECT_EQ(successful_joins, config.successful_before_failure);
}

INSTANTIATE_TEST_SUITE_P(
    Failures, PartialStartup,
    ::testing::Values(StartupCase{0, false}, StartupCase{0, true},
                      StartupCase{1, false}, StartupCase{1, true},
                      StartupCase{2, false}, StartupCase{2, true},
                      StartupCase{3, false}, StartupCase{3, true},
                      StartupCase{7, false}, StartupCase{7, true},
                      StartupCase{15, false}, StartupCase{15, true}),
    [](const ::testing::TestParamInfo<StartupCase>& info) {
      return "After" + std::to_string(info.param.successful_before_failure) +
             (info.param.factory ? "Factory" : "Constructor");
    });

TEST(ThreadStartup, IndependentThreadsCanFailStartupAndRecoverConcurrently) {
  std::vector<std::thread> callers;
  for (int index = 0; index < 16; ++index) {
    callers.emplace_back([index] {
      const int failure = index % 4;
      talon::ConcurrentTaskLimiter::Options options;
      options.max_concurrent = 4;
      {
        MeasureStartup measure(failure);
        auto result = talon::ConcurrentTaskLimiter::Create(options);
        EXPECT_EQ(result.status().code(),
                  talon::StatusCode::kResourceExhausted);
      }
      EXPECT_EQ(successful_starts, failure);
      EXPECT_EQ(successful_joins, failure);
      auto recovered = talon::ConcurrentTaskLimiter::Create(options);
      ASSERT_TRUE(recovered.ok());
      EXPECT_TRUE((*recovered)->Shutdown().ok());
    });
  }
  for (auto& caller : callers) {
    caller.join();
  }
}

TEST(ThreadStartup, InvalidConfigurationNeverAttemptsToStartAWorker) {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 0;
  {
    MeasureStartup measure(0);
    auto result = talon::ConcurrentTaskLimiter::Create(options);
    EXPECT_EQ(result.status().code(), talon::StatusCode::kInvalidArgument);
  }
  EXPECT_FALSE(start_failed);
  EXPECT_EQ(successful_starts, 0);
  EXPECT_EQ(successful_joins, 0);
}

}  // namespace

TEST(ThreadStartup, EveryPartialStartupFailureIsJoinedAndRethrown) {
  for (int started = 0; started < 4; ++started) {
    starts_before_failure = started;
    start_failed = false;
    bool caught = false;
    try {
      talon::ConcurrentTaskLimiter limiter(4);
    } catch (const std::system_error& error) {
      caught = error.code().value() == EAGAIN;
    }
    starts_before_failure = -1;
    EXPECT_TRUE(start_failed);
    EXPECT_TRUE(caught);
    talon::ConcurrentTaskLimiter recovered(1);
    EXPECT_TRUE(recovered.Schedule([] {}).ok());
    EXPECT_TRUE(recovered.Shutdown().ok());
  }
}

TEST(ThreadStartup, FactoryMapsSystemErrorWithoutLeavingWorkers) {
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = 4;
  starts_before_failure = 2;
  start_failed = false;
  auto result = talon::ConcurrentTaskLimiter::Create(options);
  starts_before_failure = -1;
  EXPECT_TRUE(start_failed);
  EXPECT_EQ(result.status().code(), talon::StatusCode::kResourceExhausted);
}
