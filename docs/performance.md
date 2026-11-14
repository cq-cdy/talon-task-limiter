# 性能设计与初始基准

## 实现成本

工作线程在构造时启动并复用。成功的 `Schedule` 分配一个任务节点，队列链接不再分配；这一点由独立的分配注入测试验证。任务存储直接接受 move-only 类型，没有为适配可复制 std::function 而额外添加 shared_ptr。

接纳、入队、出队、完成和快照使用同一个状态互斥锁，入队/出队 O(1)。用户调用与捕获对象的构造/销毁不在锁内。队列空间随 outstanding 任务数增长，不预先按额度分配巨大数组；额度并不限制用户捕获对象的字节大小。

`Submit` 另有标准 future 共享状态的分配与同步成本。本基准只测 `Schedule`，不能将其结果套用于 `Submit`。

## 可复现方法

```sh
cmake -S . -B build-release \
  -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DTALON_TASK_LIMITER_BUILD_BENCHMARKS=ON
cmake --build build-release --parallel 4

# 参数：提交线程数 工作线程数 额度 每提交线程的任务数 任务内计算轮数
build-release/talon_task_limiter_benchmark 1 1 256 200000 0
build-release/talon_task_limiter_benchmark 1 4 256 200000 0
build-release/talon_task_limiter_benchmark 8 4 256 25000 0
build-release/talon_task_limiter_benchmark 8 4 256 12500 10000
```

每组先预热一次，随后顺序测量 5 次；不同时运行其他基准或测试。计时从释放生产者起跑门闩开始，到提交完成、任务排空和 worker join 结束。执行器构造不在计时区，线程唤醒、满载拒绝、yield 重试、任务分配/销毁及校验原子操作均在计时区。

任务执行固定数量的无符号整数递推，并将结果累积到原子 checksum，防止整个计算被优化掉；完成数必须等于计划数，否则退出非零。微小任务也执行完成计数与 checksum。重试是基准负载的显式行为，库本身不重试；实际应用可以拒绝、降载或在其他队列管理重试。

## 2026-11-15 本地结果

环境：WSL2 Linux `5.15.167.4-microsoft-standard-WSL2`，x86_64，Intel Core i5-13500H，来宾报告 12 个逻辑 CPU。GCC 12.3.0、libstdc++、C++17，Release `-O3 -DNDEBUG`，无 sanitizer。未绑定 CPU、未控制宿主机调度或频率；这不是独占基准机器。

| 提交线程 | worker | 额度 | 总任务 | 计算轮数/任务 | 吞吐中位数（任务/秒） | 5 次范围 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 256 | 200,000 | 0 | 2,237,292 | 2,006,873–2,693,069 |
| 1 | 4 | 256 | 200,000 | 0 | 66,924 | 58,840–71,438 |
| 8 | 4 | 256 | 200,000 | 0 | 740,291 | 640,216–896,601 |
| 8 | 4 | 256 | 100,000 | 10,000 | 221,658 | 208,603–232,646 |

20 次原始数据（包含拒绝次数、耗时和 checksum）见 [benchmark-results.csv](benchmark-results.csv)。

这些结果建立初始实现基线，不是相对原片段或其他线程池的性能提升证明。单提交者微小任务在 4 worker 配置下显著慢于 1 worker，表明不能简单用增加线程数提升此类负载的吞吐；本次没有做系统级 profiling，因此不将差异归因于某一个具体热点。

容量、worker 数和任务粒度应依据实际应用测量选择。中央互斥锁在高竞争下可能成为瓶颈，任务饱和时频繁构造错误状态也有成本。这里不承诺公平性、尾延迟上界、实时行为或任意负载下的最佳性能。
