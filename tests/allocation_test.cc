// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <new>

#include "talon/concurrent_task_limiter.h"
#ifdef _MSC_VER
#include <malloc.h>
#endif

#include "gtest/gtest.h"

namespace {
thread_local int allocations_before_failure = -1;
thread_local bool allocation_failed = false;

void CheckAllocation() {
  if (allocations_before_failure < 0) return;
  if (allocations_before_failure-- == 0) {
    allocations_before_failure = -1;
    allocation_failed = true;
    throw std::bad_alloc();
  }
}

class FailAllocation {
 public:
  explicit FailAllocation(int index) {
    allocation_failed = false;
    allocations_before_failure = index;
  }
  ~FailAllocation() { allocations_before_failure = -1; }
  FailAllocation(const FailAllocation&) = delete;
  FailAllocation& operator=(const FailAllocation&) = delete;
};
}  // namespace

// One-shot, submitting-thread-only injection. Worker/test framework allocations
// are unaffected; no assertions run while the injection is armed.
void* operator new(std::size_t size) {
  CheckAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  CheckAllocation();
  const auto align = static_cast<std::size_t>(alignment);
#ifdef _MSC_VER
  if (void* memory = _aligned_malloc(size == 0 ? 1 : size, align))
    return memory;
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, align, size == 0 ? 1 : size) == 0) return memory;
#endif
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
#ifdef _MSC_VER
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t,
                     std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t,
                       std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
  ::operator delete(pointer, alignment);
}

namespace {
using talon::ConcurrentTaskLimiter;

TEST(Allocation, SuccessfulScheduleAllocatesExactlyOneNode) {
  ConcurrentTaskLimiter limiter(1);
  bool accepted = false;
  int remaining = -1;
  {
    FailAllocation fail(1);
    accepted = limiter.Schedule([] {}).ok();
    remaining = allocations_before_failure;
  }
  EXPECT_TRUE(accepted);
  EXPECT_FALSE(allocation_failed);
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
  EXPECT_TRUE(allocation_failed);
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
        if (result.ok()) future = std::move(*result);
      } catch (const std::bad_alloc&) {
        caught = true;
      }
    }
    EXPECT_TRUE(limiter.WaitIdle().ok());
    EXPECT_EQ(limiter.GetSnapshot().outstanding, 0U);
    if (caught) {
      EXPECT_TRUE(allocation_failed);
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
      EXPECT_TRUE(allocation_failed);
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
  EXPECT_TRUE(allocation_failed);
}
}  // namespace
