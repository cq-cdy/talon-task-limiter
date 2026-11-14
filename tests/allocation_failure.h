// SPDX-License-Identifier: Apache-2.0
#ifndef TALON_TASK_LIMITER_TESTS_ALLOCATION_FAILURE_H_
#define TALON_TASK_LIMITER_TESTS_ALLOCATION_FAILURE_H_

namespace talon_test {

// Submitting-thread-only allocation injection. The replacement allocator lives
// in a separate translation unit so optimized tests cannot inline its free()
// calls into new-expressions and misdiagnose the matched replacement pair.
class FailAllocation {
 public:
  explicit FailAllocation(int index, bool persistent = false) noexcept;
  ~FailAllocation();
  FailAllocation(const FailAllocation&) = delete;
  FailAllocation& operator=(const FailAllocation&) = delete;

  static bool Failed() noexcept;
  static int Remaining() noexcept;
};

}  // namespace talon_test

#endif  // TALON_TASK_LIMITER_TESTS_ALLOCATION_FAILURE_H_
