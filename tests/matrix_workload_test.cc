// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "talon/concurrent_task_limiter.h"
#include "talon/status_macros.h"
#include "workload_support.h"

namespace {

using talon::ConcurrentTaskLimiter;
using talon::Status;
using talon::StatusCode;
constexpr int64_t kUnwritten = std::numeric_limits<int64_t>::min();

struct MatrixCase {
  int dimension;
  int tile;
  int depth_chunk;
  int workers;
  int capacity;
  int producers;
  int fault_modulus;
  bool close_midway;
};

struct TileState {
  int row;
  int column;
  int rows;
  int columns;
  std::atomic<int> remaining{0};
  std::vector<std::vector<int64_t>> partials;
  std::vector<Status> errors;
  std::promise<Status> completion;
};

class MatrixRun {
 public:
  explicit MatrixRun(MatrixCase config)
      : config_(config),
        parts_((config.dimension + config.depth_chunk - 1) /
               config.depth_chunk),
        a_(static_cast<std::size_t>(config.dimension * config.dimension)),
        b_(a_.size()),
        output_(a_.size(), kUnwritten),
        halfway_(static_cast<std::size_t>(config.workers)) {
    for (std::size_t i = 0; i < a_.size(); ++i) {
      a_[i] = static_cast<int64_t>(talon_test::MixWord(i + 13) % 31) - 15;
      b_[i] = static_cast<int64_t>(talon_test::MixWord(i + 97) % 37) - 18;
    }
    for (int row = 0; row < config.dimension; row += config.tile) {
      for (int column = 0; column < config.dimension; column += config.tile) {
        auto tile = std::make_shared<TileState>();
        tile->row = row;
        tile->column = column;
        tile->rows = std::min(config.tile, config.dimension - row);
        tile->columns = std::min(config.tile, config.dimension - column);
        tile->remaining.store(parts_);
        tile->partials.resize(static_cast<std::size_t>(parts_));
        tile->errors.resize(static_cast<std::size_t>(parts_));
        completions_.push_back(tile->completion.get_future());
        tiles_.push_back(std::move(tile));
      }
    }
    const std::size_t jobs = tiles_.size() * static_cast<std::size_t>(parts_);
    visits_ = std::vector<std::atomic<int>>(jobs);
    accepted_.resize(jobs, 0);
    for (auto& visit : visits_) {
      visit.store(0);
    }
  }

  void Attach(ConcurrentTaskLimiter* compute, ConcurrentTaskLimiter* reduce) {
    compute_ = compute;
    reduce_ = reduce;
  }
  int jobs() const { return static_cast<int>(visits_.size()); }
  int tile_count() const { return static_cast<int>(tiles_.size()); }
  int rejections() const { return rejected_.load(); }
  void Release() { release_.Open(); }
  void WaitHalfway() { halfway_.Wait(); }

  Status Produce(int producer) {
    for (int job = producer; job < jobs(); job += config_.producers) {
      const int part = job % parts_;
      auto tile = tiles_[static_cast<std::size_t>(job / parts_)];
      auto task = [this, tile, part, job] {
        if (visits_[job].fetch_add(1) != 0) {
          duplicates_.fetch_add(1);
          return;
        }
        Arrival arrival(this, tile);
        tile->errors[part] = ComputeAndStore(tile.get(), part, job);
      };
      Status status =
          talon_test::ScheduleEventually(compute_, std::move(task), &rejected_);
      if (status.ok()) {
        accepted_[job] = 1;
      } else {
        // Unsubmitted branches also participate in fan-in. Otherwise closing
        // admission could leave the downstream result waiting forever.
        tile->errors[part] = status;
        Arrive(tile);
        if (status.code() != StatusCode::kFailedPrecondition) {
          RecordUnexpected(status);
        }
      }
    }
    return talon::OkStatus();
  }

  void Check() {
    EXPECT_EQ(duplicates_.load(), 0);
    EXPECT_EQ(unexpected_.load(), 0);
    int accepted = 0;
    for (int job = 0; job < jobs(); ++job) {
      EXPECT_EQ(visits_[job].load(), accepted_[job]) << "job=" << job;
      accepted += accepted_[job];
    }
    if (config_.close_midway) {
      EXPECT_EQ(accepted, config_.capacity);
      EXPECT_GT(rejected_.load(), 0);
    } else {
      EXPECT_EQ(accepted, jobs());
    }
    // The oracle uses the direct i/j/k product, independent of decomposition,
    // completion order, partial buffers, and downstream reduction.
    for (int index = 0; index < tile_count(); ++index) {
      auto& tile = *tiles_[index];
      talon_test::RequireReady(&completions_[index]);
      const Status status = completions_[index].get();
      StatusCode expected_code = StatusCode::kOk;
      for (int part = 0; part < parts_; ++part) {
        const int job = index * parts_ + part;
        if (accepted_[job] == 0) {
          expected_code = StatusCode::kFailedPrecondition;
          break;
        }
        if (Fails(job)) {
          expected_code = StatusCode::kDataLoss;
          break;
        }
      }
      ASSERT_EQ(status.code(), expected_code)
          << "tile=" << index << ' ' << status;
      for (int r = 0; r < tile.rows; ++r) {
        for (int c = 0; c < tile.columns; ++c) {
          const int row = tile.row + r;
          const int column = tile.column + c;
          int64_t expected = kUnwritten;
          if (expected_code == StatusCode::kOk) {
            expected = 0;
            for (int k = 0; k < config_.dimension; ++k) {
              expected += a_[row * config_.dimension + k] *
                          b_[k * config_.dimension + column];
            }
          }
          ASSERT_EQ(output_[row * config_.dimension + column], expected)
              << "row=" << row << " column=" << column;
        }
      }
    }
  }

 private:
  class Arrival {
   public:
    Arrival(MatrixRun* run, std::shared_ptr<TileState> tile)
        : run_(run), tile_(std::move(tile)) {}
    Arrival(const Arrival&) = delete;
    Arrival& operator=(const Arrival&) = delete;
    ~Arrival() { run_->Arrive(tile_); }

   private:
    MatrixRun* run_;
    std::shared_ptr<TileState> tile_;
  };

  bool Fails(int job) const {
    return config_.fault_modulus != 0 && job % config_.fault_modulus == 0;
  }

  talon::StatusOr<std::vector<int64_t>> ComputePartial(const TileState& tile,
                                                       int part, int job) {
    std::vector<int64_t> values(
        static_cast<std::size_t>(tile.rows * tile.columns), 0);
    const int begin = part * config_.depth_chunk;
    const int end = std::min(begin + config_.depth_chunk, config_.dimension);
    for (int k = begin; k < end; ++k) {
      for (int r = 0; r < tile.rows; ++r) {
        const int64_t left = a_[(tile.row + r) * config_.dimension + k];
        for (int c = 0; c < tile.columns; ++c) {
          values[r * tile.columns + c] +=
              left * b_[k * config_.dimension + tile.column + c];
        }
      }
      if (config_.close_midway && k == begin + (end - begin) / 2) {
        if (checkpoints_.fetch_add(1) < config_.workers) {
          halfway_.Arrive();
          release_.Wait();
        }
      }
    }
    if (Fails(job)) {
      return talon::DataLossError("Injected post-computation failure at job ",
                                  job);
    }
    return values;
  }

  Status ComputeAndStore(TileState* tile, int part, int job) {
    TALON_ASSIGN_OR_RETURN(auto values, ComputePartial(*tile, part, job));
    tile->partials[part] = std::move(values);
    return talon::OkStatus();
  }

  Status Reduce(const TileState& tile) {
    for (int part = 0; part < parts_; ++part) {
      TALON_RETURN_IF_ERROR(tile.errors[part]);
      if (tile.partials[part].size() !=
          static_cast<std::size_t>(tile.rows * tile.columns)) {
        return talon::InternalError("Partial matrix size mismatch");
      }
    }
    for (int r = 0; r < tile.rows; ++r) {
      for (int c = 0; c < tile.columns; ++c) {
        int64_t value = 0;
        for (int part = 0; part < parts_; ++part) {
          value += tile.partials[part][r * tile.columns + c];
        }
        output_[(tile.row + r) * config_.dimension + tile.column + c] = value;
      }
    }
    return talon::OkStatus();
  }

  void Arrive(const std::shared_ptr<TileState>& tile) {
    // Each part has one writer. The acq_rel RMW chain publishes all partials
    // to the last finisher, then queue publication publishes them to reduction.
    const int previous =
        tile->remaining.fetch_sub(1, std::memory_order_acq_rel);
    if (previous <= 0) {
      duplicates_.fetch_add(1);
      return;
    }
    if (previous == 1) {
      Status status = reduce_->Schedule(
          [this, tile] { tile->completion.set_value(Reduce(*tile)); });
      if (!status.ok()) {
        RecordUnexpected(status);
        tile->completion.set_value(std::move(status));
      }
    }
  }

  void RecordUnexpected(const Status& status) {
    unexpected_.fetch_add(1);
    ADD_FAILURE() << status;
  }

  MatrixCase config_;
  int parts_;
  std::vector<int64_t> a_;
  std::vector<int64_t> b_;
  std::vector<int64_t> output_;
  std::vector<std::shared_ptr<TileState>> tiles_;
  std::vector<std::future<Status>> completions_;
  std::vector<std::atomic<int>> visits_;
  std::vector<int> accepted_;
  std::atomic<int> checkpoints_{0};
  std::atomic<int> rejected_{0};
  std::atomic<int> unexpected_{0};
  std::atomic<int> duplicates_{0};
  talon_test::Countdown halfway_;
  talon_test::Gate release_;
  ConcurrentTaskLimiter* compute_ = nullptr;
  ConcurrentTaskLimiter* reduce_ = nullptr;
};

class MatrixPipeline : public ::testing::TestWithParam<MatrixCase> {};

TEST_P(MatrixPipeline, TiledProductFanInAndConcurrentCloseMatchSerialOracle) {
  const MatrixCase config = GetParam();
  MatrixRun run(config);
  ConcurrentTaskLimiter::Options reduce_options;
  reduce_options.max_concurrent = run.tile_count();
  reduce_options.worker_count = 2;
  // Reverse destruction order is intentional: compute may publish reductions
  // while draining, so the downstream executor must outlive it.
  ConcurrentTaskLimiter reduce(reduce_options);
  ConcurrentTaskLimiter::Options compute_options;
  compute_options.max_concurrent = config.capacity;
  compute_options.worker_count = config.workers;
  ConcurrentTaskLimiter compute(compute_options);
  run.Attach(&compute, &reduce);
  std::vector<std::thread> producers;
  talon_test::JoinThreads join(&producers);
  class ReleaseOnExit {
   public:
    explicit ReleaseOnExit(MatrixRun* run) : run_(run) {}
    ~ReleaseOnExit() { run_->Release(); }

   private:
    MatrixRun* run_;
  } release(&run);
  for (int producer = 0; producer < config.producers; ++producer) {
    producers.emplace_back(
        [&, producer] { EXPECT_TRUE(run.Produce(producer).ok()); });
  }
  if (config.close_midway) {
    run.WaitHalfway();
    talon_test::SpinUntil([&] {
      return compute.GetSnapshot().outstanding ==
                 static_cast<std::size_t>(config.capacity) &&
             run.rejections() > 0;
    });
    compute.Close();
    run.Release();
  }
  for (auto& producer : producers) {
    producer.join();
  }
  EXPECT_TRUE(compute.Shutdown().ok());
  EXPECT_TRUE(reduce.Shutdown().ok());
  run.Check();
}

INSTANTIATE_TEST_SUITE_P(
    Products, MatrixPipeline,
    ::testing::Values(MatrixCase{96, 13, 17, 2, 7, 4, 0, false},
                      MatrixCase{128, 16, 16, 4, 32, 8, 0, false},
                      MatrixCase{256, 16, 32, 8, 64, 12, 0, false},
                      MatrixCase{128, 13, 17, 4, 11, 8, 7, false},
                      MatrixCase{192, 16, 24, 4, 31, 8, 0, true},
                      MatrixCase{256, 16, 32, 8, 67, 16, 11, true}),
    [](const ::testing::TestParamInfo<MatrixCase>& info) {
      return "N" + std::to_string(info.param.dimension) + "Tile" +
             std::to_string(info.param.tile) + "Fault" +
             std::to_string(info.param.fault_modulus) +
             (info.param.close_midway ? "CloseMidway" : "Drain");
    });

}  // namespace
