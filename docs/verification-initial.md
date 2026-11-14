# 初版验证记录（31e876d）

执行日期：2026-11-15（Asia/Shanghai）。本记录仅描述初版提交 31e876d 实际执行的本地验证，不代表形式化正确性证明或所有操作系统的验证结果。

## 环境与范围

- Ubuntu 22.04 用户空间，WSL2 Linux `5.15.167.4-microsoft-standard-WSL2`，x86_64，12 个逻辑 CPU。
- CMake 3.26.5，GNU Make；GCC 11.4.0 / 12.3.0，Clang 12.0.1。
- talon-status 固定提交 `aaf333da1e5c9f494d5750f52fd0838aeb4e4205`；GoogleTest 固定提交 `f8d7d77c06936315286eb55f8de22cd23c188571`。
- 普通构建和 ASan 使用 libstdc++；完整 TSan 验证使用本地解压的 libc++ 12.0.1。
- 所有本库 targets 启用 `-Wall -Wextra -Wpedantic -Werror`；没有删除测试或减弱异常路径断言。Release 使用常规 `-DNDEBUG`，GoogleTest 断言仍然执行。

原输入只有类声明、模板提交和 CAS 片段，缺少 ThreadPool、构造/析构及构建配置，没有可直接运行的旧版基线。新仓库按明确的容量、关闭、异常和生命周期契约建立行为测试；性能数据是初始基线，不是“优化前后”的比较。

## 结果

| 验证 | 状态 | 实际结果 |
| --- | --- | --- |
| GCC 11 Debug 静态库 | **Passed** | 80/80 CTest，通过 |
| GCC 12 Release 静态库 | **Passed** | 80/80 CTest，通过 |
| GCC 12 Release 共享库 | **Passed** | 80/80 CTest，通过 |
| Clang 12 ASan + UBSan + leak detection | **Passed** | 72/72 CTest，通过 |
| GCC 12 TSan + libstdc++ | **Failed / Blocked by environment** | 71/72 通过；返回值移动抛异常测试 90 秒超时，已由无本库依赖的程序复现 |
| Clang 12 TSan + libc++ 12 | **Passed** | 完整 72/72 CTest 通过，包括同一个抛异常测试 |
| Release 打乱顺序重复运行 | **Passed** | 70 个核心测试 × 30 轮，2,100 次测试执行通过 |
| 迁移安装目录、find_package、add_subdirectory 外部消费 | **Passed** | 静态/共享构建均通过；最后修改集成测试参数传递后再次分别验证通过 |
| C++23 消费 C++17 共享库 | **Passed** | GCC 12 配置、编译及运行外部消费端成功 |
| 非法 API 用法的编译诊断 | **Passed** | 5 个预期编译失败；先验证正向基线，再检查失败诊断内容 |
| 吞吐基准 | **Passed** | 4 组负载，每组预热 + 5 次测量；全部完成数正确，原始 CSV 已保存 |
| Windows / macOS | **Not executed** | 当前环境仅 Linux；已提供 CI 配置，远端流水线尚未执行 |
| 长时间浸泡、生产业务负载、跨 CRT/任意 ABI 组合 | **Not executed** | 不在本次运行覆盖范围；应在实际部署环境追加验证 |

80 项 CTest = 70 项核心测试 + 5 项分配测试 + 2 项 Linux 线程创建失败测试 + 头文件/多 TU + 包消费 + 示例。5 项编译失败检查嵌套在包消费测试内，未重复计入 80。Sanitizer 配置保留全部 70 项核心测试，只隔离 allocator / pthread interposition 以及非 sanitizer 的安装消费测试，因此共 72 项。

## 主要命令

所有命令在仓库根目录运行。后续构建通过 FetchContent 的标准源码覆盖变量复用首次拉取的固定版本，不使用用户已有的兄弟仓库：

```sh
cmake -S . -B build-bootstrap -DCMAKE_BUILD_TYPE=Debug \
  -DTALON_TASK_LIMITER_BUILD_TESTS=OFF
cmake --build build-bootstrap --parallel 4

# 初次 build-debug 自动下载固定 GoogleTest；后续目录可复用它。
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DFETCHCONTENT_SOURCE_DIR_TALON_STATUS="$PWD/build-bootstrap/_deps/talon_status-src"
cmake --build build-debug --parallel 4
ctest --test-dir build-debug --output-on-failure --parallel 4

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DTALON_TASK_LIMITER_BUILD_BENCHMARKS=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DFETCHCONTENT_SOURCE_DIR_TALON_STATUS="$PWD/build-bootstrap/_deps/talon_status-src" \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build-debug/_deps/googletest-src"
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure --parallel 4

cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DFETCHCONTENT_SOURCE_DIR_TALON_STATUS="$PWD/build-bootstrap/_deps/talon_status-src" \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build-debug/_deps/googletest-src"
cmake --build build-shared --parallel 3
ctest --test-dir build-shared --output-on-failure --parallel 4

build-release/talon_task_limiter_test --gtest_repeat=30 --gtest_shuffle \
  --gtest_random_seed=43189 --gtest_brief=1
```

ASan/UBSan：

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-12 -DTALON_TASK_LIMITER_SANITIZER=address \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DFETCHCONTENT_SOURCE_DIR_TALON_STATUS="$PWD/build-bootstrap/_deps/talon_status-src" \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build-debug/_deps/googletest-src"
cmake --build build-asan --parallel 3
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure --parallel 2
```

C++23 安装消费（前置条件：build-shared 的 package 测试已成功生成 relocated 安装）：

```sh
cmake -S tests/consumer -B build-cxx23 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_PREFIX_PATH="$PWD/build-shared/package-check/relocated" \
  -DCHECK_COMPILE_FAILURES=ON
cmake --build build-cxx23 --parallel 2
ctest --test-dir build-cxx23 --output-on-failure
```

## TSan 超时的独立定位与完整替代验证

GCC 12 TSan 完整运行时，唯一超时是 `Submission.ThrowingResultMoveReachesFuture`。GDB 显示 worker 停在：

```text
__sanitizer::FutexWait
guard_acquire
__interceptor_pthread_once
std::call_once
std::__future_base::_State_baseV2::_M_set_result
std::promise<Result>::set_exception
FutureTask::Run
```

同一个 promise 的 `set_value` 因结果移动抛异常后，`set_exception` 再进入其 once 状态时卡住。独立程序 [tests/toolchain/once_exception.cc](../tests/toolchain/once_exception.cc) 只有两个针对同一 once_flag 的 `std::call_once` 调用，第一次抛异常；不引用本库、GoogleTest 或 future：

```sh
g++-12 -std=c++17 -pthread tests/toolchain/once_exception.cc -o build-tsan/once_plain
timeout 5s build-tsan/once_plain                 # 实测退出 0

g++-12 -std=c++17 -g -fsanitize=thread -pthread \
  tests/toolchain/once_exception.cc -o build-tsan/once_gcc
timeout 5s build-tsan/once_gcc                   # 实测退出 124：超时

clang++-12 -std=c++17 -g -fsanitize=thread -pthread \
  tests/toolchain/once_exception.cc -o build-tsan/once_clang
timeout 5s build-tsan/once_clang                 # libstdc++ 配置同样退出 124
```

因此这里有直接证据将该配置的失败定位到 TSan/标准库异常展开交互，而不是通过推测归咎环境。上游 [TSan C++ 手册](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual#faq) 也记录了 C++ 异常支持的限制。没有修改执行器异常语义，没有 skip 或 suppress 该测试。

为完成全部路径的 TSan 检查，仅下载 Ubuntu jammy 的以下六个包并解压到构建目录，没有安装或修改系统包：`libc++-12-dev`、`libc++1-12`、`libc++abi-12-dev`、`libc++abi1-12`、`libunwind-12-dev`、`libunwind-12`，全部版本为 `1:12.0.1-19ubuntu3`。它们是本地验证工具，不是本库运行依赖。

```sh
mkdir -p build-toolchain/debs
# 在 build-toolchain/debs 运行：
apt-get download libc++-12-dev=1:12.0.1-19ubuntu3 \
  libc++abi-12-dev=1:12.0.1-19ubuntu3 libc++1-12=1:12.0.1-19ubuntu3 \
  libc++abi1-12=1:12.0.1-19ubuntu3 libunwind-12-dev=1:12.0.1-19ubuntu3 \
  libunwind-12=1:12.0.1-19ubuntu3
# 对每个下载的 .deb 执行 dpkg-deb -x，目标为 build-toolchain/libcxx。

# 回到仓库根目录：
libcxx_root="$PWD/build-toolchain/libcxx/usr/lib/llvm-12"
cmake -S . -B build-tsan-libcxx -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-12 \
  "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -isystem $libcxx_root/include/c++/v1" \
  "-DCMAKE_EXE_LINKER_FLAGS=-L $libcxx_root/lib -Wl,-rpath,$libcxx_root/lib" \
  -DTALON_TASK_LIMITER_SANITIZER=thread -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DFETCHCONTENT_SOURCE_DIR_TALON_STATUS="$PWD/build-bootstrap/_deps/talon_status-src" \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build-debug/_deps/googletest-src"
cmake --build build-tsan-libcxx --parallel 4
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-tsan-libcxx --output-on-failure --parallel 2
```

独立程序在此配置下退出 0，完整测试 72/72 通过，原来超时的结果移动异常路径也通过。已安装 libc++ 的机器通常只需 `clang++ -stdlib=libc++`；CI 的 TSan lane 采用这一配置，但远端 CI 尚未执行。

## 开发中遇到并已修正的问题

- 初次自动发现了 Conda 的 GoogleTest，启动报 `GLIBCXX_3.4.30 not found`，ldd 确认载入 Conda 的旧 libstdc++。切换到固定 GoogleTest 源码、同工具链构建后，全套正常执行。
- 首次严格警告构建发现测试宏外缺少花括号导致 dangling-else 警告，已修正并重编译。
- Clang 12 揭示测试对结构化绑定的捕获依赖更高语言标准；改为普通局部变量，保持 C++17 支持。
- 本地解压 libc++ 的初次参数同时设置 `-nostdinc++` / `-stdlib=libc++`，触发 unused argument 严格警告；移除多余 `-nostdinc++` 后编译通过，未关闭严格警告。

## 证据位置与限制

本次构建目录保留 `ctest.log`、`Testing/Temporary/LastTest.log`；重复运行输出在 `build-release/stress.log`。包验证的负向编译诊断保存在 `build-*/package-check/installed/negative/*.log`。这些生成物不纳入 Git；可用上面的命令重建。基准原始 CSV 纳入版本库。

并发测试覆盖了大量可重复的竞争窗口，但不能枚举所有合法线程交错。外部对象寿命、跨执行器的循环等待、用户回调自身的数据竞争、永不结束的任务，仍须由使用者按公开契约管理。没有宣称任意用户代码无死锁或任意平台已验证。
