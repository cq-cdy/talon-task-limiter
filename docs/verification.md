# 验证记录（整体审查后）

执行日期：2026-11-15，Linux x86_64 / WSL2，CMake 3.26.5。初版 `31e876d` 的记录保留在 [verification-initial.md](verification-initial.md)；本文件对应本次修复和扩展测试。

## 实际结果

| 检查 | 状态 | 结果 |
| --- | --- | --- |
| GCC 11 Debug 静态库 | Passed | 233/233；分配注入拆分后，受影响的 11 项另外重跑通过 |
| GCC 12 Release 静态库 | Passed | 233/233 |
| GCC 12 Release 共享库 | Passed | 233/233 |
| Clang 12 ASan + UBSan + leak detection | Passed | 207/207，含安装消费与 sanitizer 运行库传递 |
| Clang 12 TSan，完整插桩 libc++/libc++abi/GTest | Passed | 207/207，无 suppression，无跳过异常路径 |
| TSan 生命周期重点回归重复 | Passed | 原先涉及的两项及 TLS 自等待回归，各连续 20 次通过 |
| Release 核心测试打乱顺序重复 | Passed | 203 项 × 30 轮 = 6,090 次测试执行 |
| C++23 外部消费 C++17 共享库 | Passed | 正向配置、编译、运行和 16 个负向编译检查 |
| 安装测试脚本的清理保护 | Passed | 受控哨兵证明错误参数不再先删除目录；无参数调用也提前失败 |
| Google 格式 / 花括号 | Static check only | 39 个 C++ 文件格式通过；19 个正常编译的 TU 通过 Google/brace 检查 |
| 生产核心高级静态分析 | Static check only | `src/concurrent_task_limiter.cc` 及对应项目头通过选定的 bugprone / Clang Analyzer 检查 |
| Windows / macOS / 远端 CI | Not executed | 已提供配置，当前环境没有执行这些平台 |
| LLVM 自身完整测试套件 | Not executed | 这里只构建测试工具链，并以独立 probe 和本项目测试验证所需路径 |

233 = 203 项核心测试 + 11 项分配故障 + 16 项 Linux 线程启动测试 + 头文件/多 TU + 安装消费 + 示例。Sanitizer 配置保留全部 203 核心项，隔离自定义 allocator / pthread interposition，另运行头文件、裸 sanitizer 消费端、安装消费和示例，共 207 项。16 个编译失败检查包含在安装消费测试内，不重复计数。

单次大负载测试包含 262,144 个任务；30 轮仅这一项就核对了 7,864,320 次执行和对应资源释放。还包含 288 个并发历史、48 组构造/回滚矩阵、128 层嵌套以及跨池任务树，详见 [review.md](review.md)。没有禁用原有断言或删除关键路径测试。

## 常规构建与运行

沿用初版记录中的固定依赖源码缓存。以下是本次实际使用的配置/命令组合；所有目录都在仓库内。

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
cmake --build build-debug --parallel 4
ctest --test-dir build-debug --output-on-failure --parallel 4

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DTALON_TASK_LIMITER_BUILD_BENCHMARKS=ON
cmake --build build-release --parallel 3
ctest --test-dir build-release --output-on-failure --parallel 4

cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
cmake --build build-shared --parallel 3
ctest --test-dir build-shared --output-on-failure --parallel 4

build-release/talon_task_limiter_test --gtest_repeat=30 --gtest_shuffle \
  --gtest_random_seed=43189 --gtest_brief=1
```

ASan/UBSan 使用 Clang 12，`TALON_TASK_LIMITER_SANITIZER=address`：

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure --parallel 2
```

安装消费测试会迁移安装前缀，再配置独立消费工程；路径列表包含带空格的额外目录。负向编译检查先证明正常源码可以编译链接，然后要求每个非法用例失败且匹配预期诊断。

## TSan 的完整工具链验证

初版使用未插桩的发行版 libc++，72 项测试曾通过。本次扩大覆盖后，实际有两项报告 future 共享状态释放与 `pthread_cond_wait` 的访问缺少同步：旧配置 **205/207 通过、2 项失败**。这些失败记录保留，没有用重跑直到通过替代调查。

独立的 [future_lifetime.cc](../tests/toolchain/future_lifetime.cc) 只使用标准 promise/future/thread，不包含本库、GTest、自定义 allocator 或额外同步。相同未插桩 libc++ 配置下，它以 TSan 错误退出码 66 复现同样报告。检查 libc++ 源码可见共享计数的原子递减位于未插桩的库实现内；结合独立复现，定位为 TSan 看不到该同步关系。

随后按 [LLVM 12 构建机制](https://releases.llvm.org/12.0.1/projects/libcxx/docs/BuildingLibcxx.html) 在本地编译 libc++ 和 libc++abi，均启用 `-fsanitize=thread`。GTest 和项目代码同样插桩。独立 probe 的 2,000 轮通过，完整项目 207/207 通过。没有给库或测试增加延迟、额外等待或 sanitizer suppression。

```sh
python3 tools/build_tsan_runtime.py --jobs 4
runtime_prefix="$PWD/build-toolchain/tsan-runtime"
cmake -S . -B build-tsan-full -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-12 -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -fsanitize=thread -fno-omit-frame-pointer -isystem $runtime_prefix/include/c++/v1" \
  "-DCMAKE_EXE_LINKER_FLAGS=-L $runtime_prefix/lib -Wl,-rpath,$runtime_prefix/lib" \
  -DTALON_TASK_LIMITER_SANITIZER=thread
cmake --build build-tsan-full --parallel 4
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-tsan-full --output-on-failure --parallel 2
```

脚本固定 LLVM 12.0.1 三个源码归档的 SHA-256，仅使用 LLVM 的 CMake 模块，不重建编译器。安装位置始终是工作目录中的 `tsan-runtime`，未修改系统 libc++。LLVM 12 的文本链接脚本不能执行 ELF RPATH 重写，构建脚本为该运行库设置 `CMAKE_SKIP_RPATH=ON`，由消费端提供运行库搜索路径。

GCC/libstdc++ 的 TSan `call_once` 异常展开超时仍可由 [once_exception.cc](../tests/toolchain/once_exception.cc) 独立复现；本次未重复跑已知会超时的整套配置，完整验证使用上述工具链。

## 失败到通过的记录

| 原始失败 | 原因与处理 | 当前结果 |
| --- | --- | --- |
| 空函数指针提交返回 OK 后退出 139 | 发布前没有检查可空输入；增加校验与额度回滚 | 回归 Passed |
| TLS 退出回调回归 `timeout 5s` 退出 124 | worker 标记清除过早 | 回归 Passed |
| sanitizer 外部消费链接失败 | 静态库没有传递运行库链接参数 | 构建和运行 Passed |
| 多前缀消费配置失败 | 分号参数被拆分 | 安装消费 Passed |
| 错误构建目录先清理后报错 | 未检查参数和构建标识 | 受控临时目录回归 Passed |
| Release `-Werror=mismatched-new-delete` | 替换 allocator 同 TU 内联后的 GCC 诊断 | 拆分 TU，保留警告，全部分配测试 Passed |
| 未插桩 libc++ 的两项 TSan 报告 | 独立标准库程序复现；引用计数同步不可见 | 完整插桩后 Passed |
| libc++ 安装 RPATH_CHANGE 失败 | CMake 对文本链接脚本做 ELF 修改 | 调整本地工具链配置后安装 Passed |

## 静态检查的范围

```sh
python3 tools/check_style.py --build-dir build-debug
clang-tidy-12 src/concurrent_task_limiter.cc -p build-debug \
  --extra-arg=-std=c++17 \
  --checks='-*,google-*,readability-braces-around-statements,bugprone-use-after-move,bugprone-exception-escape,bugprone-dangling-handle,clang-analyzer-*'
```

风格检查包括所有正常项目 TU，编译失败文件检查格式；Windows 条件分支的花括号另作源码检查。高级分析结果仅指生产核心和其头文件，不冒充并发或运行时验证。

也执行过全测试 TU 的高级分析。其诊断包括故意抛异常/自身销毁的测试、验证拒绝不移动参数时的 moved-from 报告，以及 talon-status 错误态被错误地继续推导为 value 态的跨过程路径。后者与依赖的 `CheckError` 和 status/object 生命周期不变量矛盾；源码复核及动态验证未支持这些路径为真实缺陷。宽范围诊断保留在 `build-debug/review-tidy.log`，没有改写第三方代码或削弱测试来消除它们。

## 证据与适用边界

本次日志位于 `build-debug/review-ctest.log`、各配置 `review-run.log`、`build-release/review-stress.log`、`build-debug/style-check.log` 及各构建的 `Testing/Temporary`。故障注入隔离、完整工具链要求与平台限制均是验证范围的一部分。性能原始 CSV 纳入 Git，见 [performance.md](performance.md)。

这些检查不能穷尽所有调度、任意编译器或任意用户回调。外部生命周期、共享数据同步、返回借用引用/view/指针的寿命及应用自身等待依赖仍须符合 [公开契约](../README.md)；也没有把有限基准解释为所有负载下的性能最优。
