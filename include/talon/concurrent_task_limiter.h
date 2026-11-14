// SPDX-License-Identifier: Apache-2.0
#ifndef TALON_CONCURRENT_TASK_LIMITER_H_
#define TALON_CONCURRENT_TASK_LIMITER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

#include "talon/status.h"
#include "talon/status_or.h"
#include "talon/task_limiter/export.h"
#include "talon/task_limiter/internal/task.h"

namespace talon {

// A bounded, owning executor. The bound includes task construction, queued
// tasks, execution, exception handling, and destruction of captured objects.
//
// All operations may run concurrently except construction/destruction. The
// owner must keep this object alive until every external call has returned.
// Tasks must eventually finish; no operation forcibly interrupts user code.
class ConcurrentTaskLimiter final {
 public:
  struct Options {
    int32_t max_concurrent = 1;
    // Zero selects max_concurrent workers. Otherwise require 1..max_concurrent.
    int32_t worker_count = 0;
    // Called on the worker for exceptions escaping Schedule tasks. Calls may
    // overlap; the handler must be thread-safe and must not throw. An empty
    // handler, or an exception escaping a handler, calls std::terminate().
    // Submit delivers exceptions to its future instead.
    std::function<void(std::exception_ptr)> on_unhandled_exception;
  };

  struct Snapshot {
    int32_t max_concurrent;
    // Configured worker count; does not become zero after Shutdown.
    int32_t worker_count;
    bool accepting;
    std::size_t outstanding;
    std::size_t queued;
    std::size_t running;
    // outstanding - queued - running counts admissions still being constructed.
  };

  // Throws invalid_argument for invalid options, bad_alloc on allocation
  // failure, or system_error if workers cannot be started. Partial startup is
  // rolled back and joined before an exception escapes.
  TALON_TASK_LIMITER_EXPORT explicit ConcurrentTaskLimiter(
      int32_t max_concurrent);
  TALON_TASK_LIMITER_EXPORT explicit ConcurrentTaskLimiter(Options options);

  ConcurrentTaskLimiter(const ConcurrentTaskLimiter&) = delete;
  ConcurrentTaskLimiter& operator=(const ConcurrentTaskLimiter&) = delete;
  ConcurrentTaskLimiter(ConcurrentTaskLimiter&&) = delete;
  ConcurrentTaskLimiter& operator=(ConcurrentTaskLimiter&&) = delete;

  // Closes, drains and joins. Destruction from this executor's own task,
  // exception handler, or submission callback is a contract violation and
  // terminates rather than deadlocking or accessing freed state.
  TALON_TASK_LIMITER_EXPORT ~ConcurrentTaskLimiter();

  // Status-based construction. Converting an error to Status may itself
  // allocate; this is not a no-throw API under complete memory exhaustion.
  [[nodiscard]] static StatusOr<std::unique_ptr<ConcurrentTaskLimiter>> Create(
      Options options) {
    // Keep StatusOr in the caller's translation unit: its AUTO backend depends
    // on the language mode, so it must not cross the compiled library boundary.
    Status status = ValidateOptions(options);
    if (!status.ok()) {
      return status;
    }
    try {
      return std::make_unique<ConcurrentTaskLimiter>(std::move(options));
    } catch (const std::bad_alloc&) {
      return ResourceExhaustedError("Cannot allocate executor resources");
    } catch (const std::system_error& error) {
      return ResourceExhaustedError("Cannot start executor workers: ",
                                    error.what());
    }
  }

  // Immediate admission: does not wait for capacity, but may contend on a
  // mutex. Full -> ResourceExhausted; closed -> FailedPrecondition. Rejection
  // does not copy/move func or args. Construction exceptions propagate to the
  // caller and release the reserved slot. A successful reservation is accepted
  // even if Close races with subsequent task construction.
  // A null raw callable, member pointer, or standard member target returns
  // InvalidArgument after admission succeeds, without publishing a task.
  template <typename Func, typename... Args>
  [[nodiscard]] Status Schedule(Func&& func, Args&&... args) {
    static_assert(
        std::is_invocable_v<std::decay_t<Func>, std::decay_t<Args>...>,
        "Task must be invocable with decayed rvalues; use std::ref "
        "for borrowed lvalues");
    Status status = TryAcquire();
    if (!status.ok()) {
      return status;
    }
    Reservation reservation(this);
    if (task_limiter_internal::HasNullInvocationTarget(func, args...)) {
      return InvalidArgumentError("Task callable or member target is null");
    }
    auto task = std::make_unique<task_limiter_internal::TaskFor<Func, Args...>>(
        std::forward<Func>(func), std::forward<Args>(args)...);
    Enqueue(std::move(task));
    reservation.Commit();
    return OkStatus();
  }

  // Admission status and asynchronous result are separate. Supports void,
  // references and move-only results. A ready future does not imply that task
  // captures have been destroyed; WaitIdle/Shutdown provide that guarantee.
  template <typename Func, typename... Args>
  [[nodiscard]] auto Submit(Func&& func, Args&&... args) -> StatusOr<
      std::future<task_limiter_internal::ResultFor<Func, Args...>>> {
    using Result = task_limiter_internal::ResultFor<Func, Args...>;
    Status status = TryAcquire();
    if (!status.ok()) {
      return status;
    }
    Reservation reservation(this);
    if (task_limiter_internal::HasNullInvocationTarget(func, args...)) {
      return InvalidArgumentError("Task callable or member target is null");
    }
    auto task = std::make_unique<task_limiter_internal::FutureTask<
        Result, std::decay_t<Func>, std::decay_t<Args>...>>(
        std::forward<Func>(func), std::forward<Args>(args)...);
    auto future = task->GetFuture();
    Enqueue(std::move(task));
    reservation.Commit();
    return future;
  }

  // Idempotent; allowed from tasks and their callbacks. Already reserved tasks
  // still run. This does not wait or cancel tasks.
  TALON_TASK_LIMITER_EXPORT void Close();

  // Waits for an observed idle point, including capture destruction. New
  // admissions may follow unless Close has been called. Same-executor tasks
  // and submission callbacks get FailedPrecondition instead of self-waiting.
  // Worker thread_local destruction is covered by Shutdown, not by WaitIdle.
  [[nodiscard]] TALON_TASK_LIMITER_EXPORT Status WaitIdle();
  [[nodiscard]] TALON_TASK_LIMITER_EXPORT Status
  WaitIdleUntil(std::chrono::steady_clock::time_point deadline);

  // Close + drain + join, including worker thread_local destruction. Concurrent
  // callers are supported. Same-executor callbacks (also at thread exit) get
  // FailedPrecondition without changing the accepting state.
  [[nodiscard]] TALON_TASK_LIMITER_EXPORT Status Shutdown();

  // One coherent observation, not a capacity reservation.
  [[nodiscard]] TALON_TASK_LIMITER_EXPORT Snapshot GetSnapshot() const;

 private:
  class TALON_TASK_LIMITER_EXPORT Reservation {
   public:
    explicit Reservation(ConcurrentTaskLimiter* owner) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation();
    void Commit() noexcept { committed_ = true; }
    static bool Contains(const ConcurrentTaskLimiter* owner) noexcept;

   private:
    static thread_local Reservation* current_;
    ConcurrentTaskLimiter* owner_;
    Reservation* previous_;
    bool committed_ = false;
  };

  class Impl;

  [[nodiscard]] TALON_TASK_LIMITER_EXPORT static Status ValidateOptions(
      const Options& options);
  [[nodiscard]] TALON_TASK_LIMITER_EXPORT Status TryAcquire();
  TALON_TASK_LIMITER_EXPORT void ReleaseReservation() noexcept;
  TALON_TASK_LIMITER_EXPORT void Enqueue(
      std::unique_ptr<task_limiter_internal::Task> task);
  bool IsSelfWait() const noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace talon

#endif  // TALON_CONCURRENT_TASK_LIMITER_H_
