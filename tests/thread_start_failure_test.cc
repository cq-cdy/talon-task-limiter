// SPDX-License-Identifier: Apache-2.0
// Linux-only ELF interposition, in a separate non-sanitized process.
#include <dlfcn.h>
#include <pthread.h>

#include <cerrno>
#include <cstdlib>
#include <system_error>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"

namespace {
thread_local int starts_before_failure = -1;
thread_local bool start_failed = false;
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
  if (!real_create) std::abort();
  return real_create(thread, attributes, entry, argument);
}

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
