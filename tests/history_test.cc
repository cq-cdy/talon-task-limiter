// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "test_support.h"

namespace {

using talon::ConcurrentTaskLimiter;
using talon::StatusCode;

enum class OperationKind { kSchedule, kClose, kSnapshot };

struct Operation {
  OperationKind kind = OperationKind::kSchedule;
  int begin = 0;
  int end = 0;
  StatusCode code = StatusCode::kOk;
  bool accepting = true;
  std::size_t outstanding = 0;
};

// This is an abstract API history checker, independent of queue/lock internals.
// Tasks are held behind a gate, so the only model transitions are admission
// and close. Search all serializations respecting observed real-time ordering.
class HistoryChecker {
 public:
  HistoryChecker(const std::vector<Operation>& operations, int capacity)
      : operations_(operations),
        capacity_(capacity),
        predecessors_(operations.size(), 0),
        failed_(std::size_t{1} << operations.size(), false) {
    for (std::size_t i = 0; i < operations_.size(); ++i) {
      for (std::size_t j = 0; j < operations_.size(); ++j) {
        if (operations_[j].end < operations_[i].begin) {
          predecessors_[i] |= uint32_t{1} << j;
        }
      }
    }
  }

  bool IsLinearizable() { return Search(0, 0, true); }

 private:
  bool Search(uint32_t completed, int outstanding, bool accepting) {
    if (completed + 1 == failed_.size()) {
      return true;
    }
    if (failed_[completed]) {
      return false;
    }
    for (std::size_t i = 0; i < operations_.size(); ++i) {
      const uint32_t bit = uint32_t{1} << i;
      if ((completed & bit) != 0 ||
          (completed & predecessors_[i]) != predecessors_[i]) {
        continue;
      }
      const Operation& operation = operations_[i];
      int next_outstanding = outstanding;
      bool next_accepting = accepting;
      switch (operation.kind) {
        case OperationKind::kSchedule: {
          const StatusCode expected =
              !accepting                 ? StatusCode::kFailedPrecondition
              : outstanding == capacity_ ? StatusCode::kResourceExhausted
                                         : StatusCode::kOk;
          if (operation.code != expected) {
            continue;
          }
          if (expected == StatusCode::kOk) {
            ++next_outstanding;
          }
          break;
        }
        case OperationKind::kClose:
          next_accepting = false;
          break;
        case OperationKind::kSnapshot:
          if (operation.accepting != accepting ||
              operation.outstanding != static_cast<std::size_t>(outstanding)) {
            continue;
          }
          break;
      }
      if (Search(completed | bit, next_outstanding, next_accepting)) {
        return true;
      }
    }
    // Model state is determined by this subset: accepted admissions and close
    // operations have fixed recorded outcomes, independent of their ordering.
    failed_[completed] = true;
    return false;
  }

  const std::vector<Operation>& operations_;
  const int capacity_;
  std::vector<uint32_t> predecessors_;
  std::vector<bool> failed_;
};

TEST(HistoryChecker, RejectsImpossibleCapacityAndRealTimeOrder) {
  std::vector<Operation> over_capacity{
      {OperationKind::kSchedule, 0, 3, StatusCode::kOk, true, 0},
      {OperationKind::kSchedule, 1, 4, StatusCode::kOk, true, 0}};
  EXPECT_FALSE(HistoryChecker(over_capacity, 1).IsLinearizable());
  std::vector<Operation> after_close{
      {OperationKind::kClose, 0, 1, StatusCode::kOk, true, 0},
      {OperationKind::kSchedule, 2, 3, StatusCode::kOk, true, 0}};
  EXPECT_FALSE(HistoryChecker(after_close, 2).IsLinearizable());
  after_close[1].code = StatusCode::kFailedPrecondition;
  EXPECT_TRUE(HistoryChecker(after_close, 2).IsLinearizable());
}

TEST(HistoryChecker, AcceptsOverlappingAdmissionThatReturnsAfterClose) {
  const std::vector<Operation> history{
      {OperationKind::kSchedule, 0, 5, StatusCode::kOk, true, 0},
      {OperationKind::kClose, 1, 2, StatusCode::kOk, true, 0},
      {OperationKind::kSnapshot, 3, 4, StatusCode::kOk, false, 1}};
  EXPECT_TRUE(HistoryChecker(history, 1).IsLinearizable());
}

struct HistoryCase {
  int capacity;
  int workers;
  int seed;
};

std::vector<HistoryCase> HistoryCases() {
  std::vector<HistoryCase> cases;
  for (const auto& size : std::array<std::array<int, 2>, 3>{
           std::array<int, 2>{1, 1}, {3, 2}, {8, 4}}) {
    for (int seed = 0; seed < 12; ++seed) {
      cases.push_back({size[0], size[1], seed});
    }
  }
  return cases;
}

class AdmissionHistory : public ::testing::TestWithParam<HistoryCase> {};

TEST_P(AdmissionHistory, RealConcurrentHistoriesHaveLegalSerializations) {
  const HistoryCase config = GetParam();
  for (int round = 0; round < 8; ++round) {
    constexpr int kProducers = 4;
    std::vector<Operation> history(2 * kProducers + 1);
    std::atomic<int> clock{0};
    std::atomic<int> executed{0};
    talon_test::Gate start;
    talon_test::Gate release;
    ConcurrentTaskLimiter::Options options;
    options.max_concurrent = config.capacity;
    options.worker_count = config.workers;
    ConcurrentTaskLimiter limiter(options);
    talon_test::OpenOnExit cleanup(release);
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
      producers.emplace_back([&, producer] {
        std::minstd_rand random(static_cast<unsigned>(1 + config.seed * 313 +
                                                      round * 17 + producer));
        start.Wait();
        const unsigned turns = random() % 8;
        for (unsigned turn = 0; turn < turns; ++turn) {
          std::this_thread::yield();
        }
        Operation& admission = history[2 * producer];
        admission.kind = OperationKind::kSchedule;
        admission.begin = clock.fetch_add(1);
        admission.code = limiter
                             .Schedule([&] {
                               release.Wait();
                               executed.fetch_add(1);
                             })
                             .code();
        admission.end = clock.fetch_add(1);

        Operation& observation = history[2 * producer + 1];
        observation.kind = OperationKind::kSnapshot;
        observation.begin = clock.fetch_add(1);
        const auto snapshot = limiter.GetSnapshot();
        observation.end = clock.fetch_add(1);
        observation.accepting = snapshot.accepting;
        observation.outstanding = snapshot.outstanding;
        EXPECT_LE(snapshot.queued + snapshot.running, snapshot.outstanding);
        EXPECT_LE(snapshot.running, static_cast<std::size_t>(config.workers));
      });
    }
    std::thread closer([&] {
      start.Wait();
      for (int turn = 0; turn < config.seed + round; ++turn) {
        std::this_thread::yield();
      }
      Operation& closing = history.back();
      closing.kind = OperationKind::kClose;
      closing.begin = clock.fetch_add(1);
      limiter.Close();
      closing.end = clock.fetch_add(1);
    });
    start.Open();
    for (auto& producer : producers) {
      producer.join();
    }
    closer.join();

    std::ostringstream trace;
    int accepted = 0;
    for (const Operation& operation : history) {
      trace << static_cast<int>(operation.kind) << ':' << operation.begin << '-'
            << operation.end << ' ' << static_cast<int>(operation.code) << ' '
            << operation.accepting << '/' << operation.outstanding << '\n';
      if (operation.kind == OperationKind::kSchedule &&
          operation.code == StatusCode::kOk) {
        ++accepted;
      }
    }
    EXPECT_TRUE(HistoryChecker(history, config.capacity).IsLinearizable())
        << "seed=" << config.seed << " round=" << round << '\n'
        << trace.str();
    EXPECT_EQ(limiter.GetSnapshot().outstanding,
              static_cast<std::size_t>(accepted));
    release.Open();
    EXPECT_TRUE(limiter.Shutdown().ok());
    EXPECT_EQ(executed.load(), accepted);
  }
}

INSTANTIATE_TEST_SUITE_P(Models, AdmissionHistory,
                         ::testing::ValuesIn(HistoryCases()),
                         [](const ::testing::TestParamInfo<HistoryCase>& info) {
                           return "Capacity" +
                                  std::to_string(info.param.capacity) +
                                  "Workers" +
                                  std::to_string(info.param.workers) + "Seed" +
                                  std::to_string(info.param.seed);
                         });

}  // namespace
