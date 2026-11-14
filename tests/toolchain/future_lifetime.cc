// SPDX-License-Identifier: Apache-2.0
// Independent probe for sanitizer visibility of standard-library shared state.
// No task limiter, GoogleTest, custom allocator, or user synchronization.
#include <future>
#include <thread>
#include <utility>

int main() {
  for (int round = 0; round < 2000; ++round) {
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    std::thread producer(
        [promise = std::move(promise)]() mutable { promise.set_value(); });
    future.get();
    producer.join();
  }
}
