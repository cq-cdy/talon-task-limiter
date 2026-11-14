// SPDX-License-Identifier: Apache-2.0
#ifndef TALON_TASK_LIMITER_INTERNAL_TASK_H_
#define TALON_TASK_LIMITER_INTERNAL_TASK_H_

#include <exception>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace talon {
namespace task_limiter_internal {

// An intrusive queue avoids a second allocation when publishing a task. Only
// the executor accesses next; payload construction/destruction happens
// unlocked.
class Task {
 public:
  virtual ~Task() = default;
  virtual void Run() = 0;
  std::unique_ptr<Task> TakeNext() noexcept { return std::move(next_); }
  void SetNext(std::unique_ptr<Task> next) noexcept { next_ = std::move(next); }

 private:
  std::unique_ptr<Task> next_;
};

template <typename Target>
bool IsNullMemberTarget(const Target&) noexcept {
  return false;
}

template <typename Target>
bool IsNullMemberTarget(Target* target) noexcept {
  return target == nullptr;
}

template <typename Target, typename Deleter>
bool IsNullMemberTarget(
    const std::unique_ptr<Target, Deleter>& target) noexcept {
  return target == nullptr;
}

template <typename Target>
bool IsNullMemberTarget(const std::shared_ptr<Target>& target) noexcept {
  return target == nullptr;
}

// Only inspect nullable standard invocation forms. Arbitrary user-defined
// dereference/conversion operators retain their own validity contracts.
template <typename Func, typename... Args>
bool HasNullInvocationTarget(const Func& func, const Args&... args) noexcept {
  if constexpr (std::is_pointer_v<Func> || std::is_member_pointer_v<Func>) {
    if (func == nullptr) {
      return true;
    }
  }
  if constexpr (std::is_member_pointer_v<Func> && sizeof...(Args) != 0) {
    return IsNullMemberTarget(std::get<0>(std::tie(args...)));
  }
  return false;
}

template <typename Func, typename... Args>
class BoundTask final : public Task {
  static_assert(std::is_nothrow_destructible_v<Func> &&
                    (std::is_nothrow_destructible_v<Args> && ...),
                "Task captures must have noexcept destructors");

 public:
  template <typename F, typename... A>
  explicit BoundTask(F&& func, A&&... args)
      : func_(std::forward<F>(func)), args_(std::forward<A>(args)...) {}

  void Run() override {
    // Match std::thread: decay-copy at submission, then invoke once as rvalues.
    // std::ref/std::cref explicitly opt in to borrowed arguments or callables.
    static_cast<void>(std::apply(std::move(func_), std::move(args_)));
  }

 private:
  Func func_;
  std::tuple<Args...> args_;
};

template <typename Result, typename Func, typename... Args>
class FutureTask final : public Task {
  static_assert(!std::is_rvalue_reference_v<Result>,
                "std::future does not support rvalue reference results");
  static_assert(std::is_void_v<Result> || std::is_lvalue_reference_v<Result> ||
                    (std::is_move_constructible_v<Result> &&
                     std::is_nothrow_destructible_v<Result>),
                "Future value results must be move-constructible and have "
                "noexcept destructors");
  static_assert(std::is_nothrow_destructible_v<Func> &&
                    (std::is_nothrow_destructible_v<Args> && ...),
                "Task captures must have noexcept destructors");

 public:
  template <typename F, typename... A>
  explicit FutureTask(F&& func, A&&... args)
      : func_(std::forward<F>(func)), args_(std::forward<A>(args)...) {}

  std::future<Result> GetFuture() { return promise_.get_future(); }

  void Run() override {
    try {
      if constexpr (std::is_void_v<Result>) {
        std::apply(std::move(func_), std::move(args_));
        promise_.set_value();
      } else {
        promise_.set_value(std::apply(std::move(func_), std::move(args_)));
      }
    } catch (...) {
      promise_.set_exception(std::current_exception());
    }
  }

 private:
  // Captures are owned by this node, not the future's shared state. Keeping a
  // future alive must not extend capture lifetimes past WaitIdle/Shutdown.
  Func func_;
  std::tuple<Args...> args_;
  std::promise<Result> promise_;
};

template <typename Func, typename... Args>
using TaskFor = BoundTask<std::decay_t<Func>, std::decay_t<Args>...>;

template <typename Func, typename... Args>
using ResultFor =
    std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>;

}  // namespace task_limiter_internal
}  // namespace talon

#endif  // TALON_TASK_LIMITER_INTERNAL_TASK_H_
