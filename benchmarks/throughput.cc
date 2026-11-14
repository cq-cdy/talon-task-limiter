// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "talon/concurrent_task_limiter.h"

namespace {
int Parse(const char* text) {
  std::string_view view(text);
  int value = 0;
  const auto result =
      std::from_chars(view.data(), view.data() + view.size(), value);
  if (result.ec != std::errc() || result.ptr != view.data() + view.size() ||
      value < 0) {
    throw std::invalid_argument("Expected nonnegative integer");
  }
  return value;
}

struct alignas(64) ProducerStats {
  std::uint64_t rejected = 0;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr
        << "Usage: " << argv[0]
        << " producers workers capacity tasks_per_producer work_iterations\n";
    return 1;
  }
  try {
    const int producers = Parse(argv[1]);
    const int workers = Parse(argv[2]);
    const int capacity = Parse(argv[3]);
    const int tasks = Parse(argv[4]);
    const int work = Parse(argv[5]);
    if (producers == 0 || producers > 256 || workers == 0 || workers > 1024 ||
        capacity < workers || tasks == 0) {
      throw std::invalid_argument(
          "Require 1..256 producers, 1..1024 workers, capacity >= workers, "
          "tasks > 0");
    }
    talon::ConcurrentTaskLimiter::Options options;
    options.max_concurrent = capacity;
    options.worker_count = workers;
    talon::ConcurrentTaskLimiter limiter(options);
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> checksum{0};
    std::vector<ProducerStats> stats(static_cast<std::size_t>(producers));
    std::vector<std::thread> threads;
    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool start = false;
    auto produce = [&](int producer) {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_cv.wait(lock, [&] { return start; });
      }
      for (int task = 0; task < tasks; ++task) {
        const auto id = static_cast<std::uint64_t>(producer) * tasks + task;
        for (;;) {
          auto status = limiter.Schedule([&, id] {
            std::uint64_t value = id + 1;
            for (int step = 0; step < work; ++step)
              value = value * 1664525 + 1013904223;
            checksum.fetch_xor(value, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_relaxed);
          });
          if (status.ok()) break;
          if (status.code() != talon::StatusCode::kResourceExhausted)
            std::terminate();
          ++stats[static_cast<std::size_t>(producer)].rejected;
          std::this_thread::yield();
        }
      }
    };
    try {
      for (int i = 0; i < producers; ++i) threads.emplace_back(produce, i);
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(start_mutex);
        start = true;
      }
      start_cv.notify_all();
      for (auto& thread : threads) thread.join();
      throw;
    }
    const auto begin = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(start_mutex);
      start = true;
    }
    start_cv.notify_all();
    for (auto& thread : threads) thread.join();
    const auto status = limiter.Shutdown();
    const auto end = std::chrono::steady_clock::now();
    if (!status.ok() ||
        completed != static_cast<std::uint64_t>(producers) * tasks)
      return 2;
    const double seconds = std::chrono::duration<double>(end - begin).count();
    std::uint64_t rejected = 0;
    for (const auto& stat : stats) rejected += stat.rejected;
    std::cout << "producers,workers,capacity,tasks_per_producer,work,completed,"
                 "rejections,seconds,tasks_per_second,checksum\n"
              << producers << ',' << workers << ',' << capacity << ',' << tasks
              << ',' << work << ',' << completed << ',' << rejected << ','
              << std::fixed << std::setprecision(6) << seconds << ','
              << completed.load() / seconds << ',' << checksum << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
