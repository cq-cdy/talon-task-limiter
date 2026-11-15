# 验证记录（工具宏与复合工作负载）

执行日期：2026-11-15。此前整体审查的数据保留在 [verification-review.md](verification-review.md)，初版记录见 [verification-initial.md](verification-initial.md)。本轮没有把历史通过次数算作新测试结果。

## 本轮结果

| 配置 / 检查 | 状态 | 实际结果 |
| --- | --- | --- |
| GCC 11 Debug 静态库 | Passed | 256/256 |
| GCC 12 Release 静态库 | Passed | 256/256 |
| GCC 12 Release 共享库 | Passed | 256/256 |
| Clang 12 ASan/UBSan、leak detection | Passed | 230/230，包含全部 17 项工作负载 |
| Clang 12 TSan、完整插桩 libc++/libc++abi/GTest | Passed | 230/230，包含全部 17 项工作负载 |
| Release 工作负载打乱顺序重复 | Passed | 17 项 × 5 轮 = 85 次复合工作负载执行 |
| C++23 外部消费 | Passed | 配置、编译、运行通过；17 个负向编译用例通过 |
| Google 格式 / 花括号 | Static check only | 47 个 C++ 文件、25 个正常 TU 通过 |
| 宏展开的 ODR 检查 | Passed | 不同 __COUNTER__ 历史下，预处理后的完整公共类定义相同 |
| 共享符号检查 | Static check only | 本机生成头及 nm 动态符号表已检查 |
| Windows/macOS 原生运行、远端 CI | Not executed | 当前仅在 Linux/WSL2 实际执行 |

256 = 208 项核心测试 + 17 项复合工作负载 + 11 项分配注入 + 16 项 Linux 线程启动测试 + 头文件/多 TU + 宏 ODR + 安装消费 + 示例。Sanitizer 隔离 allocator / pthread interposition，同时增加裸 sanitizer 消费端，共 230 项。17 项预期编译失败嵌套在安装消费测试内，不重复计数。

## 宏改写的验证重点

- 原来的状态传播改为依赖提供的 TALON_RETURN_IF_ERROR；示例和实际数据计算辅助函数使用 TALON_ASSIGN_OR_RETURN。
- 检查表达式只求值一次、二进制错误消息保持完整、move-only future/值正确转移、早返回执行 RAII，并且满载失败不归还其他任务占用的额度。
- CMake target 和安装导出传递 TALON_STATUS_USE_LINE_COUNTER=1；显式选择 0 的消费者有独立的预期编译失败测试。
- 两个源文件分别直接包含公共头、先展开 32 次 __COUNTER__ 再包含公共头，预处理后比较完整公共定义。检查位于 cmake/CheckMacroOdr.cmake。
- 消费端先定义自己的短名字宏、先包含 Talon 宏头，再包含执行器头；验证库没有覆盖 RETURN_IF_ERROR/ASSIGN_OR_RETURN。

这是一项编译配置变化，手动集成和每行 ASSIGN 的要求见 [macros-and-exports.md](macros-and-exports.md)。

## 工作负载及规模

完整场景说明见 [workloads.md](workloads.md)。本次实际执行了：

- 262,144 和 1,048,576 个复合 CPU 作业，混合 Schedule/Submit；大场景保留 262,144 个 future，逐任务对照参考摘要，核对 payload 清理及存活上界。
- 六种矩阵流水线：规则/不规则边界分块、独立归并执行器、真实乘加计算、业务错误及计算中途 Close。
- 五种跨三个池的数据 DAG，包含每阶段 4,194,304 条记录、连续四阶段的最大成功场景；比较分片排序/k-way merge 与整批串行排序，核对每个阶段节点。
- 四种析构驱动链：最多八条链 × 96 步 × 16,384 个 64-bit 字，验证重入、外部背压调度、Close 和部分完成结果。

测试中的整数算法使用确定输入和精确 oracle；不是只数任务次数，也不以 sleep 猜测调度顺序。关闭阈值不会被误当作实际 Close 的线性化时间。

## 命令与日志

已在现有固定依赖配置上执行：

```sh
cmake --build build-debug --parallel 3
cmake --build build-release --parallel 3
cmake --build build-shared --parallel 3
cmake --build build-asan --parallel 3
cmake --build build-tsan-full --parallel 3

ctest --test-dir build-debug --output-on-failure --parallel 4
ctest --test-dir build-release --output-on-failure --parallel 4
ctest --test-dir build-shared --output-on-failure --parallel 4

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure --parallel 2
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-tsan-full --output-on-failure --parallel 2

python3 tools/check_style.py --build-dir build-debug
build-release/talon_task_limiter_workload_test \
  --gtest_repeat=5 --gtest_shuffle --gtest_random_seed=73129 --gtest_brief=1
```

TSan 使用本地构建、完整插桩的标准库，不使用 suppression；工具链和构建命令见 [README](../README.md) 及之前的验证记录。

各配置日志为 `build-*/workload-upgrade.log`，重复运行记录为 `build-release/workload-repeat.log`；宏预处理证据在 `build-*/macro-odr/{plain,shifted}.ii`。最后补充外部生产者启动失败时的门闩清理后，受影响的两项批量负载又在 ASan/UBSan 和完整 TSan 下重跑通过，日志为对应目录中的 `bulk-final.log`。安装后的正向消费和 17 项负向编译检查都已运行。共享库导出宏的实际定义来自生成的 export.h，动态表通过 `nm -D -C` 检查；没有把 Windows 的生成规则描述成 Windows 运行验证。

这次任务是可读性和验证覆盖扩展，没有据此声称调度性能提升或任意用户代码不会发生依赖死锁。公开生命周期和异常边界仍然适用。
