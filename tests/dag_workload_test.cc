// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "talon/status_macros.h"
#include "workload_support.h"

namespace {

struct Record {
  uint32_t key;
  uint32_t id;
};

bool LessRecord(const Record& left, const Record& right) {
  if (left.key != right.key) {
    return left.key < right.key;
  }
  return left.id < right.id;
}

struct Cursor {
  Record record;
  int branch;
  std::size_t index;
};

class GreaterCursor {
 public:
  bool operator()(const Cursor& left, const Cursor& right) const {
    return LessRecord(right.record, left.record);
  }
};

uint32_t TransformKey(const Record& record, int level, int group) {
  return static_cast<uint32_t>(talon_test::MixWord(
      record.key ^ (static_cast<uint64_t>(record.id) << 32) ^
      static_cast<uint64_t>(level + 1) * UINT64_C(0x9e3779b97f4a7c15) ^
      static_cast<uint64_t>(group + 1)));
}

struct DagCase {
  int records;
  int groups;
  int levels;
  int branches;
  int workers;
  int fault_level;
};

struct GroupState {
  int id;
  std::shared_ptr<const std::vector<Record>> initial;
  std::vector<Record> result;
  std::promise<talon::Status> completion;
  std::atomic<bool> terminal{false};
};

struct StageState {
  std::shared_ptr<GroupState> group;
  std::shared_ptr<const std::vector<Record>> input;
  int level;
  std::atomic<int> remaining{0};
  std::vector<std::vector<Record>> sorted;
  std::vector<talon::Status> errors;
};

class SortDag {
 public:
  explicit SortDag(DagCase config)
      : config_(config),
        visits_(static_cast<std::size_t>(config.groups * config.levels *
                                         (config.branches + 1))) {
    for (auto& visit : visits_) {
      visit.store(0);
    }
    for (int group = 0; group < config.groups; ++group) {
      auto initial = std::make_shared<std::vector<Record>>();
      initial->reserve(static_cast<std::size_t>(config.records));
      for (int id = 0; id < config.records; ++id) {
        initial->push_back({static_cast<uint32_t>(talon_test::MixWord(
                                static_cast<uint64_t>(id) + 17 * group)),
                            static_cast<uint32_t>(id)});
      }
      auto state = std::make_shared<GroupState>();
      state->id = group;
      state->initial = initial;
      futures_.push_back(state->completion.get_future());
      groups_.push_back(std::move(state));
    }
  }

  void Attach(
      const std::vector<std::unique_ptr<talon::ConcurrentTaskLimiter>>& pools) {
    for (const auto& pool : pools) {
      pools_.push_back(pool.get());
    }
  }
  int task_bound() const { return static_cast<int>(visits_.size()); }

  void Start() {
    for (const auto& group : groups_) {
      StartStage(group, group->initial, 0);
    }
  }

  void Wait() {
    for (auto& future : futures_) {
      talon_test::RequireReady(&future);
    }
  }

  void Check() {
    EXPECT_EQ(unexpected_.load(), 0);
    for (int group = 0; group < config_.groups; ++group) {
      const talon::Status status = futures_[group].get();
      const bool failed = config_.fault_level >= 0;
      EXPECT_EQ(status.code(),
                failed ? talon::StatusCode::kDataLoss : talon::StatusCode::kOk);
      if (failed) {
        EXPECT_EQ(status.message(), "map failure " + std::to_string(group) +
                                        "/" +
                                        std::to_string(config_.fault_level));
      }
      for (int level = 0; level < config_.levels; ++level) {
        const int expected = !failed || level <= config_.fault_level ? 1 : 0;
        for (int branch = 0; branch <= config_.branches; ++branch) {
          EXPECT_EQ(visits_[Index(group, level, branch)].load(), expected)
              << "group=" << group << " level=" << level
              << " branch=" << branch;
        }
      }
      if (!failed) {
        std::vector<Record> reference = *groups_[group]->initial;
        for (int level = 0; level < config_.levels; ++level) {
          for (auto& record : reference) {
            record.key = TransformKey(record, level, group);
          }
          // Whole-dataset sort is independent of partitioning and k-way merge.
          std::sort(reference.begin(), reference.end(), LessRecord);
        }
        const auto& actual = groups_[group]->result;
        ASSERT_EQ(actual.size(), reference.size());
        for (std::size_t index = 0; index < reference.size(); ++index) {
          ASSERT_EQ(actual[index].key, reference[index].key)
              << "record=" << index;
          ASSERT_EQ(actual[index].id, reference[index].id)
              << "record=" << index;
        }
      }
    }
  }

 private:
  int Index(int group, int level, int branch) const {
    return (group * config_.levels + level) * (config_.branches + 1) + branch;
  }

  bool Visit(int index) {
    if (visits_[index].fetch_add(1) != 0) {
      unexpected_.fetch_add(1);
      return false;
    }
    return true;
  }

  talon::StatusOr<std::vector<Record>> MapAndSort(const StageState& stage,
                                                  int branch) {
    const std::size_t begin = stage.input->size() *
                              static_cast<std::size_t>(branch) /
                              static_cast<std::size_t>(config_.branches);
    const std::size_t end = stage.input->size() *
                            static_cast<std::size_t>(branch + 1) /
                            static_cast<std::size_t>(config_.branches);
    std::vector<Record> output;
    output.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
      Record record = (*stage.input)[index];
      record.key = TransformKey(record, stage.level, stage.group->id);
      output.push_back(record);
    }
    std::sort(output.begin(), output.end(), LessRecord);
    if (stage.level == config_.fault_level &&
        branch == stage.group->id % config_.branches) {
      return talon::DataLossError("map failure ", stage.group->id, "/",
                                  stage.level);
    }
    return output;
  }

  talon::Status SaveShard(StageState* stage, int branch) {
    TALON_ASSIGN_OR_RETURN(auto sorted, MapAndSort(*stage, branch));
    stage->sorted[branch] = std::move(sorted);
    return talon::OkStatus();
  }

  talon::StatusOr<std::vector<Record>> Merge(const StageState& stage) {
    for (const auto& status : stage.errors) {
      TALON_RETURN_IF_ERROR(status);
    }
    std::priority_queue<Cursor, std::vector<Cursor>, GreaterCursor> ready;
    for (int branch = 0; branch < config_.branches; ++branch) {
      if (!stage.sorted[branch].empty()) {
        const Record first = stage.sorted[branch][0];
        ready.push({first, branch, 0});
      }
    }
    std::vector<Record> merged;
    merged.reserve(stage.input->size());
    while (!ready.empty()) {
      const Cursor cursor = ready.top();
      ready.pop();
      merged.push_back(cursor.record);
      const int branch = cursor.branch;
      const std::size_t next = cursor.index + 1;
      if (next < stage.sorted[branch].size()) {
        const Record record = stage.sorted[branch][next];
        ready.push({record, branch, next});
      }
    }
    if (merged.size() != stage.input->size()) {
      return talon::DataLossError("Merge lost records");
    }
    return merged;
  }

  talon::Status Advance(const std::shared_ptr<StageState>& stage) {
    TALON_ASSIGN_OR_RETURN(auto merged, Merge(*stage));
    if (stage->level + 1 == config_.levels) {
      stage->group->result = std::move(merged);
      Finish(stage->group, talon::OkStatus());
    } else {
      StartStage(stage->group,
                 std::make_shared<const std::vector<Record>>(std::move(merged)),
                 stage->level + 1);
    }
    return talon::OkStatus();
  }

  void StartStage(const std::shared_ptr<GroupState>& group,
                  std::shared_ptr<const std::vector<Record>> input, int level) {
    auto stage = std::make_shared<StageState>();
    stage->group = group;
    stage->input = std::move(input);
    stage->level = level;
    stage->remaining.store(config_.branches);
    stage->sorted.resize(static_cast<std::size_t>(config_.branches));
    stage->errors.resize(static_cast<std::size_t>(config_.branches));
    for (int branch = 0; branch < config_.branches; ++branch) {
      const talon::Status status =
          pools_[level % 3]->Schedule([this, stage, branch] {
            if (!Visit(Index(stage->group->id, stage->level, branch))) {
              return;
            }
            stage->errors[branch] = SaveShard(stage.get(), branch);
            Arrive(stage);
          });
      if (!status.ok()) {
        unexpected_.fetch_add(1);
        stage->errors[branch] = status;
        Arrive(stage);
      }
    }
  }

  void Arrive(const std::shared_ptr<StageState>& stage) {
    // The last acq_rel decrement observes every preceding branch publication.
    if (stage->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      const talon::Status status =
          pools_[(stage->level + 1) % 3]->Schedule([this, stage] {
            if (!Visit(
                    Index(stage->group->id, stage->level, config_.branches))) {
              return;
            }
            talon::Status result = Advance(stage);
            if (!result.ok()) {
              Finish(stage->group, std::move(result));
            }
          });
      if (!status.ok()) {
        unexpected_.fetch_add(1);
        Finish(stage->group, status);
      }
    }
  }

  void Finish(const std::shared_ptr<GroupState>& group, talon::Status status) {
    if (group->terminal.exchange(true)) {
      unexpected_.fetch_add(1);
      return;
    }
    group->completion.set_value(std::move(status));
  }

  DagCase config_;
  std::vector<std::atomic<int>> visits_;
  std::vector<std::shared_ptr<GroupState>> groups_;
  std::vector<std::future<talon::Status>> futures_;
  std::vector<talon::ConcurrentTaskLimiter*> pools_;
  std::atomic<int> unexpected_{0};
};

class CpuDataDag : public ::testing::TestWithParam<DagCase> {};

TEST_P(CpuDataDag, MultiPoolMapSortMergeChainsMatchWholeDatasetReference) {
  const DagCase config = GetParam();
  SortDag run(config);
  std::vector<std::unique_ptr<talon::ConcurrentTaskLimiter>> pools;
  for (int pool = 0; pool < 3; ++pool) {
    talon::ConcurrentTaskLimiter::Options options;
    options.max_concurrent = run.task_bound();
    options.worker_count = config.workers;
    pools.push_back(std::make_unique<talon::ConcurrentTaskLimiter>(options));
  }
  talon_test::DrainExecutors cleanup(&pools);
  run.Attach(pools);
  run.Start();
  run.Wait();
  // Terminal results imply no more graph edges can be published. Shutdown
  // still drains callbacks and captures before the backing data is inspected.
  for (auto& pool : pools) {
    EXPECT_TRUE(pool->Shutdown().ok());
  }
  run.Check();
}

INSTANTIATE_TEST_SUITE_P(DataGraphs, CpuDataDag,
                         ::testing::Values(DagCase{131071, 3, 5, 7, 3, -1},
                                           DagCase{262144, 4, 6, 16, 4, -1},
                                           DagCase{1048576, 4, 4, 16, 4, -1},
                                           DagCase{131071, 3, 7, 11, 3, 0},
                                           DagCase{262144, 4, 6, 16, 4, 3}),
                         [](const ::testing::TestParamInfo<DagCase>& info) {
                           return "Records" +
                                  std::to_string(info.param.records) +
                                  "Stages" + std::to_string(info.param.levels) +
                                  "Groups" + std::to_string(info.param.groups) +
                                  (info.param.fault_level < 0
                                       ? "Success"
                                       : "Fault" + std::to_string(
                                                       info.param.fault_level));
                         });

}  // namespace
