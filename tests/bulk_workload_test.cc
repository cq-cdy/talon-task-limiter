// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "talon/status_macros.h"
#include "workload_support.h"

namespace {

constexpr int kWords = 16;
using Words = std::array<uint64_t, kWords>;

Words GenerateWords(int id) {
  Words values{};
  for (int i = 0; i < kWords; ++i) {
    values[i] = talon_test::MixWord(static_cast<uint64_t>(id) *
                                        UINT64_C(0x9e3779b97f4a7c15) +
                                    static_cast<uint64_t>(i));
  }
  return values;
}

uint64_t FoldWords(const Words& values) {
  uint64_t result = UINT64_C(0x123456789abcdef0);
  for (uint64_t value : values) {
    result = (result ^ value) * UINT64_C(0x100000001b3);
  }
  return result;
}

uint64_t ComputeFingerprint(Words values) {
  for (int i = 0; i < kWords; ++i) {
    for (int round = 0; round < 4; ++round) {
      values[i] = talon_test::MixWord(values[i] + static_cast<uint64_t>(round));
    }
  }
  std::sort(values.begin(), values.end());
  return FoldWords(values);
}

// Independent sort and loop ordering: do not validate the parallel path by
// merely calling the task body a second time.
uint64_t ReferenceFingerprint(int id) {
  Words values = GenerateWords(id);
  for (int round = 0; round < 4; ++round) {
    for (auto& value : values) {
      value = talon_test::MixWord(value + static_cast<uint64_t>(round));
    }
  }
  for (int i = 1; i < kWords; ++i) {
    const uint64_t value = values[i];
    int position = i;
    while (position > 0 && values[position - 1] > value) {
      values[position] = values[position - 1];
      --position;
    }
    values[position] = value;
  }
  return FoldWords(values);
}

struct PayloadStats {
  std::atomic<int> alive{0};
  std::atomic<int> peak{0};
  std::atomic<int> destroyed{0};
};

class Payload {
 public:
  Payload(int id, PayloadStats* stats)
      : values_(GenerateWords(id)), stats_(stats) {
    talon_test::UpdatePeak(stats_->peak, stats_->alive.fetch_add(1) + 1);
  }
  Payload(const Payload&) = delete;
  Payload& operator=(const Payload&) = delete;
  ~Payload() {
    stats_->alive.fetch_sub(1);
    stats_->destroyed.fetch_add(1);
  }
  const Words& values() const { return values_; }

 private:
  Words values_;
  PayloadStats* stats_;
};

struct BulkCase {
  int tasks;
  int producers;
  int workers;
  int capacity;
};

class BulkCpuWorkload : public ::testing::TestWithParam<BulkCase> {};

TEST_P(BulkCpuWorkload, EveryCompositeResultAndPayloadIsAccountedFor) {
  const BulkCase config = GetParam();
  PayloadStats payload_stats;
  std::vector<std::atomic<int>> visits(static_cast<std::size_t>(config.tasks));
  std::vector<std::atomic<uint64_t>> digests(
      static_cast<std::size_t>(config.tasks));
  for (int id = 0; id < config.tasks; ++id) {
    visits[id].store(0);
    digests[id].store(0);
  }
  // Retain one quarter of the futures through WaitIdle: captures must still
  // be released promptly, independently of the shared result's lifetime.
  std::vector<std::future<uint64_t>> futures(
      static_cast<std::size_t>(config.tasks / 4));
  std::vector<talon::Status> producer_status(
      static_cast<std::size_t>(config.producers));
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::atomic<bool> released{false};
  talon_test::Gate start;
  talon_test::Gate release;
  talon::ConcurrentTaskLimiter::Options options;
  options.max_concurrent = config.capacity;
  options.worker_count = config.workers;
  talon::ConcurrentTaskLimiter limiter(options);
  std::vector<std::thread> producers;
  talon_test::JoinThreads join(&producers);
  talon_test::OpenOnExit start_cleanup(start);
  talon_test::OpenOnExit cleanup(release);

  auto produce = [&](int producer) -> talon::Status {
    start.Wait();
    for (int id = producer; id < config.tasks; id += config.producers) {
      auto task = [&, id,
                   payload = std::make_unique<Payload>(id, &payload_stats)] {
        if (!released.load(std::memory_order_acquire)) {
          release.Wait();
        }
        const uint64_t digest = ComputeFingerprint(payload->values());
        digests[id].store(digest, std::memory_order_relaxed);
        visits[id].fetch_add(1, std::memory_order_relaxed);
        return digest;
      };
      if (id % 4 == 0) {
        TALON_ASSIGN_OR_RETURN(
            auto future,
            talon_test::SubmitEventually(&limiter, std::move(task), &rejected));
        futures[id / 4] = std::move(future);
      } else {
        TALON_RETURN_IF_ERROR(talon_test::ScheduleEventually(
            &limiter, std::move(task), &rejected));
      }
      accepted.fetch_add(1, std::memory_order_relaxed);
    }
    return talon::OkStatus();
  };
  for (int producer = 0; producer < config.producers; ++producer) {
    producers.emplace_back(
        [&, producer] { producer_status[producer] = produce(producer); });
  }
  start.Open();
  talon_test::SpinUntil([&] {
    return limiter.GetSnapshot().outstanding ==
               static_cast<std::size_t>(config.capacity) &&
           rejected.load() > 0;
  });
  EXPECT_EQ(limiter.GetSnapshot().outstanding,
            static_cast<std::size_t>(config.capacity));
  released.store(true, std::memory_order_release);
  release.Open();
  for (auto& producer : producers) {
    producer.join();
  }
  ASSERT_TRUE(limiter.WaitIdle().ok());
  EXPECT_EQ(payload_stats.alive.load(), 0);
  EXPECT_EQ(payload_stats.destroyed.load(), config.tasks);
  EXPECT_LE(payload_stats.peak.load(), config.capacity + config.producers);
  EXPECT_GT(rejected.load(), 0);
  ASSERT_TRUE(limiter.Shutdown().ok());
  for (const auto& status : producer_status) {
    ASSERT_TRUE(status.ok()) << status;
  }
  ASSERT_EQ(accepted.load(), config.tasks);
  for (int id = 0; id < config.tasks; ++id) {
    ASSERT_EQ(visits[id].load(), 1) << "task=" << id;
    const uint64_t expected = ReferenceFingerprint(id);
    ASSERT_EQ(digests[id].load(), expected) << "task=" << id;
    if (id % 4 == 0) {
      ASSERT_TRUE(futures[id / 4].valid());
      ASSERT_EQ(futures[id / 4].get(), expected) << "task=" << id;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(LargeBatches, BulkCpuWorkload,
                         ::testing::Values(BulkCase{262144, 16, 4, 64},
                                           BulkCase{1048576, 32, 8, 256}),
                         [](const ::testing::TestParamInfo<BulkCase>& info) {
                           return "Tasks" + std::to_string(info.param.tasks) +
                                  "Workers" +
                                  std::to_string(info.param.workers);
                         });

}  // namespace
