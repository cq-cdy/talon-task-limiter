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

class ConcurrentTaskLimiter::Impl {
 public:
  explicit Impl(ConcurrentTaskLimiter* owner, Options options)
      : max_concurrent_(options.max_concurrent),
        worker_count_(options.worker_count == 0 ? options.max_concurrent
                                                : options.worker_count),
        exception_handler_(std::move(options.on_unhandled_exception)) {
    workers_.reserve(static_cast<std::size_t>(worker_count_));
    try {
      for (int32_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this, owner] { WorkerLoop(owner); });
      }
    } catch (...) {
      // A throwing constructor does not run ~Impl. Join every worker already
      // started before allowing mutexes and condition variables to be
      // destroyed.
      Close();
      for (auto& worker : workers_) {
        worker.join();
      }
      throw;
    }
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_ = false;
    }
    work_ready_.notify_all();
  }

 private:
  friend class ConcurrentTaskLimiter;

  // Called with mutex_ held, after either rollback or payload destruction.
  void ReleaseSlot() noexcept {
    assert(outstanding_ > 0);
    --outstanding_;
    if (outstanding_ == 0) {
      idle_.notify_all();
      // On close, workers must wait for constructing reservations too. A
      // rollback may be the final transition that lets them exit.
      if (!accepting_) {
        work_ready_.notify_all();
      }
    }
  }

  void WorkerLoop(const ConcurrentTaskLimiter* owner) noexcept {
    current_executor = owner;
    for (;;) {
      std::unique_ptr<task_limiter_internal::Task> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_ready_.wait(lock, [this] {
          return head_ != nullptr || (!accepting_ && outstanding_ == 0);
        });
        if (!head_) {
          break;
        }
        task = std::move(head_);
        head_ = task->TakeNext();
        if (!head_) {
          tail_ = nullptr;
        }
        --queued_;
        ++running_;
      }
      try {
        task->Run();
      } catch (...) {
        // Schedule has no future. Never silently discard its exceptions.
        if (!exception_handler_) {
          std::terminate();
        }
        try {
          exception_handler_(std::current_exception());
        } catch (...) {
          std::terminate();
        }
      }
      // A callable's destructor may reenter the executor. Keep the worker
      // marker and its slot until all user-owned captures have been destroyed.
      task.reset();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --running_;
        ReleaseSlot();
      }
    }
    // Keep the marker through user thread_local destructors. They run after
    // WorkerLoop returns and may reenter Shutdown while an external caller is
    // joining this thread. The owner remains alive until that join completes;
    // this trivially destructible TLS pointer then disappears with the thread.
  }

  const int32_t max_concurrent_;
  const int32_t worker_count_;
  const std::function<void(std::exception_ptr)> exception_handler_;
  mutable std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable idle_;
  bool accepting_ = true;
  std::size_t outstanding_ = 0;
  std::size_t queued_ = 0;
  std::size_t running_ = 0;
  std::unique_ptr<task_limiter_internal::Task> head_;
  task_limiter_internal::Task* tail_ = nullptr;

  // Only external Shutdown callers acquire join_mutex_, then mutex_ (via
  // Close). Workers never acquire join_mutex_, including their self-wait error
  // path.
  std::mutex join_mutex_;
  std::vector<std::thread> workers_;
};

thread_local ConcurrentTaskLimiter::Reservation*
    ConcurrentTaskLimiter::Reservation::current_ = nullptr;

ConcurrentTaskLimiter::Reservation::Reservation(
    ConcurrentTaskLimiter* owner) noexcept
    : owner_(owner), previous_(current_) {
  current_ = this;
}

ConcurrentTaskLimiter::Reservation::~Reservation() {
  if (!committed_) {
    owner_->ReleaseReservation();
  }
  current_ = previous_;
}

bool ConcurrentTaskLimiter::Reservation::Contains(
    const ConcurrentTaskLimiter* owner) noexcept {
  for (auto* reservation = current_; reservation != nullptr;
       reservation = reservation->previous_) {
    if (reservation->owner_ == owner) {
      return true;
    }
  }
  return false;
}

ConcurrentTaskLimiter::ConcurrentTaskLimiter(int32_t max_concurrent)
    : ConcurrentTaskLimiter(WithLimit(max_concurrent)) {}

ConcurrentTaskLimiter::ConcurrentTaskLimiter(Options options) {
  const Status status = ValidateOptions(options);
  if (!status.ok()) {
    // Compatibility boundary for the original throwing constructor. Callers
    // that handle configuration errors should use Create(), which validates
    // before entering this path and returns Status directly.
    throw std::invalid_argument(status.message());
  }
  impl_ = std::make_unique<Impl>(this, std::move(options));
}

Status ConcurrentTaskLimiter::ValidateOptions(const Options& options) {
  if (options.max_concurrent <= 0) {
    return InvalidArgumentError("max_concurrent must be positive");
  }
  if (options.worker_count < 0 ||
      options.worker_count > options.max_concurrent) {
    return InvalidArgumentError("worker_count must be in [1, max_concurrent]");
  }
  return OkStatus();
}

ConcurrentTaskLimiter::~ConcurrentTaskLimiter() {
  if (IsSelfWait()) {
    std::terminate();
  }
  static_cast<void>(Shutdown());
}

Status ConcurrentTaskLimiter::TryAcquire() {
  bool closed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    closed = !impl_->accepting_;
    if (!closed && impl_->outstanding_ <
                       static_cast<std::size_t>(impl_->max_concurrent_)) {
      ++impl_->outstanding_;
      return OkStatus();
    }
  }
  // Error message construction can allocate; keep it outside the state lock.
  if (closed) {
    return FailedPreconditionError("Task limiter is closed");
  }
  return ResourceExhaustedError("Maximum outstanding task limit (",
                                impl_->max_concurrent_, ") reached");
}

void ConcurrentTaskLimiter::ReleaseReservation() noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  impl_->ReleaseSlot();
}

void ConcurrentTaskLimiter::Enqueue(
    std::unique_ptr<task_limiter_internal::Task> task) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    auto* new_tail = task.get();
    if (impl_->tail_) {
      impl_->tail_->SetNext(std::move(task));
    } else {
      impl_->head_ = std::move(task);
    }
    impl_->tail_ = new_tail;
    ++impl_->queued_;
  }
  impl_->work_ready_.notify_one();
}

bool ConcurrentTaskLimiter::IsSelfWait() const noexcept {
  return current_executor == this || Reservation::Contains(this);
}

void ConcurrentTaskLimiter::Close() { impl_->Close(); }

Status ConcurrentTaskLimiter::WaitIdle() {
  if (IsSelfWait()) {
    return FailedPreconditionError("Cannot wait from this executor's callback");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex_);
  impl_->idle_.wait(lock, [this] { return impl_->outstanding_ == 0; });
  return OkStatus();
}

Status ConcurrentTaskLimiter::WaitIdleUntil(
    std::chrono::steady_clock::time_point deadline) {
  if (IsSelfWait()) {
    return FailedPreconditionError("Cannot wait from this executor's callback");
  }
  std::unique_lock<std::mutex> lock(impl_->mutex_);
  if (impl_->idle_.wait_until(lock, deadline,
                              [this] { return impl_->outstanding_ == 0; })) {
    return OkStatus();
  }
  lock.unlock();
  return DeadlineExceededError("Tasks are still outstanding");
}

Status ConcurrentTaskLimiter::Shutdown() {
  // Must precede join_mutex_ acquisition: an external joiner can already be
  // waiting for this worker, and waiting on its mutex_ would deadlock.
  if (IsSelfWait()) {
    return FailedPreconditionError(
        "Cannot shut down from this executor's callback");
  }
  std::lock_guard<std::mutex> join_lock(impl_->join_mutex_);
  Close();
  for (auto& worker : impl_->workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  return OkStatus();
}

ConcurrentTaskLimiter::Snapshot ConcurrentTaskLimiter::GetSnapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return {impl_->max_concurrent_, impl_->worker_count_, impl_->accepting_,
          impl_->outstanding_,    impl_->queued_,       impl_->running_};
}

}  // namespace talon
