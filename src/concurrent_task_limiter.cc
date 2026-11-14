// SPDX-License-Identifier: Apache-2.0
#include "talon/concurrent_task_limiter.h"

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace talon {
namespace {
thread_local const ConcurrentTaskLimiter* current_executor = nullptr;

ConcurrentTaskLimiter::Options WithLimit(int32_t max_concurrent) {
  ConcurrentTaskLimiter::Options options;
  options.max_concurrent = max_concurrent;
  return options;
}
}  // namespace

struct ConcurrentTaskLimiter::Impl {
  explicit Impl(ConcurrentTaskLimiter* owner, Options options)
      : max_concurrent(options.max_concurrent),
        worker_count(options.worker_count == 0 ? options.max_concurrent
                                               : options.worker_count),
        exception_handler(std::move(options.on_unhandled_exception)) {
    if (max_concurrent <= 0) {
      throw std::invalid_argument("max_concurrent must be positive");
    }
    if (worker_count <= 0 || worker_count > max_concurrent) {
      throw std::invalid_argument(
          "worker_count must be in [1, max_concurrent]");
    }
    workers.reserve(static_cast<std::size_t>(worker_count));
    try {
      for (int32_t i = 0; i < worker_count; ++i) {
        workers.emplace_back([this, owner] { WorkerLoop(owner); });
      }
    } catch (...) {
      // A throwing constructor does not run ~Impl. Join every worker already
      // started before allowing mutexes and condition variables to be
      // destroyed.
      Close();
      for (auto& worker : workers) worker.join();
      throw;
    }
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      accepting = false;
    }
    work_ready.notify_all();
  }

  // Called with mutex held, after either rollback or payload destruction.
  void ReleaseSlot() noexcept {
    assert(outstanding > 0);
    --outstanding;
    if (outstanding == 0) {
      idle.notify_all();
      // On close, workers must wait for constructing reservations too. A
      // rollback may be the final transition that lets them exit.
      if (!accepting) work_ready.notify_all();
    }
  }

  void WorkerLoop(const ConcurrentTaskLimiter* owner) noexcept {
    current_executor = owner;
    for (;;) {
      std::unique_ptr<task_limiter_internal::Task> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        work_ready.wait(lock, [this] {
          return head != nullptr || (!accepting && outstanding == 0);
        });
        if (!head) break;
        task = std::move(head);
        head = std::move(task->next);
        if (!head) tail = nullptr;
        --queued;
        ++running;
      }
      try {
        task->Run();
      } catch (...) {
        // Schedule has no future. Never silently discard its exceptions.
        if (!exception_handler) std::terminate();
        try {
          exception_handler(std::current_exception());
        } catch (...) {
          std::terminate();
        }
      }
      // A callable's destructor may reenter the executor. Keep the worker
      // marker and its slot until all user-owned captures have been destroyed.
      task.reset();
      {
        std::lock_guard<std::mutex> lock(mutex);
        --running;
        ReleaseSlot();
      }
    }
    current_executor = nullptr;
  }

  const int32_t max_concurrent;
  const int32_t worker_count;
  const std::function<void(std::exception_ptr)> exception_handler;
  mutable std::mutex mutex;
  std::condition_variable work_ready;
  std::condition_variable idle;
  bool accepting = true;
  std::size_t outstanding = 0;
  std::size_t queued = 0;
  std::size_t running = 0;
  std::unique_ptr<task_limiter_internal::Task> head;
  task_limiter_internal::Task* tail = nullptr;

  // Only external Shutdown callers acquire join_mutex, then mutex (via Close).
  // Workers never acquire join_mutex, including their self-wait error path.
  std::mutex join_mutex;
  std::vector<std::thread> workers;
};

thread_local ConcurrentTaskLimiter::Reservation*
    ConcurrentTaskLimiter::Reservation::current_ = nullptr;

ConcurrentTaskLimiter::Reservation::Reservation(
    ConcurrentTaskLimiter* owner) noexcept
    : owner_(owner), previous_(current_) {
  current_ = this;
}

ConcurrentTaskLimiter::Reservation::~Reservation() {
  if (!committed_) owner_->ReleaseReservation();
  current_ = previous_;
}

bool ConcurrentTaskLimiter::Reservation::Contains(
    const ConcurrentTaskLimiter* owner) noexcept {
  for (auto* reservation = current_; reservation != nullptr;
       reservation = reservation->previous_) {
    if (reservation->owner_ == owner) return true;
  }
  return false;
}

ConcurrentTaskLimiter::ConcurrentTaskLimiter(int32_t max_concurrent)
    : ConcurrentTaskLimiter(WithLimit(max_concurrent)) {}

ConcurrentTaskLimiter::ConcurrentTaskLimiter(Options options)
    : impl_(std::make_unique<Impl>(this, std::move(options))) {}

ConcurrentTaskLimiter::~ConcurrentTaskLimiter() {
  if (IsSelfWait()) std::terminate();
  static_cast<void>(Shutdown());
}

Status ConcurrentTaskLimiter::TryAcquire() {
  bool closed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    closed = !impl_->accepting;
    if (!closed &&
        impl_->outstanding < static_cast<std::size_t>(impl_->max_concurrent)) {
      ++impl_->outstanding;
      return OkStatus();
    }
  }
  // Error message construction can allocate; keep it outside the state lock.
  if (closed) return FailedPreconditionError("Task limiter is closed");
  return ResourceExhaustedError("Maximum outstanding task limit (",
                                impl_->max_concurrent, ") reached");
}

void ConcurrentTaskLimiter::ReleaseReservation() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->ReleaseSlot();
}

void ConcurrentTaskLimiter::Enqueue(
    std::unique_ptr<task_limiter_internal::Task> task) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* tail = task.get();
    if (impl_->tail) {
      impl_->tail->next = std::move(task);
    } else {
      impl_->head = std::move(task);
    }
    impl_->tail = tail;
    ++impl_->queued;
  }
  impl_->work_ready.notify_one();
}

bool ConcurrentTaskLimiter::IsSelfWait() const noexcept {
  return current_executor == this || Reservation::Contains(this);
}

void ConcurrentTaskLimiter::Close() { impl_->Close(); }

Status ConcurrentTaskLimiter::WaitIdle() {
  if (IsSelfWait()) {
    return FailedPreconditionError("Cannot wait from this executor's callback");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->idle.wait(lock, [this] { return impl_->outstanding == 0; });
  return OkStatus();
}

Status ConcurrentTaskLimiter::WaitIdleUntil(
    std::chrono::steady_clock::time_point deadline) {
  if (IsSelfWait()) {
    return FailedPreconditionError("Cannot wait from this executor's callback");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (impl_->idle.wait_until(lock, deadline,
                             [this] { return impl_->outstanding == 0; })) {
    return OkStatus();
  }
  lock.unlock();
  return DeadlineExceededError("Tasks are still outstanding");
}

Status ConcurrentTaskLimiter::Shutdown() {
  // Must precede join_mutex acquisition: an external joiner can already be
  // waiting for this worker, and waiting on its mutex would deadlock.
  if (IsSelfWait()) {
    return FailedPreconditionError(
        "Cannot shut down from this executor's callback");
  }
  std::lock_guard<std::mutex> join_lock(impl_->join_mutex);
  Close();
  for (auto& worker : impl_->workers) {
    if (worker.joinable()) worker.join();
  }
  return OkStatus();
}

ConcurrentTaskLimiter::Snapshot ConcurrentTaskLimiter::GetSnapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return {impl_->max_concurrent, impl_->worker_count, impl_->accepting,
          impl_->outstanding,    impl_->queued,       impl_->running};
}

}  // namespace talon
