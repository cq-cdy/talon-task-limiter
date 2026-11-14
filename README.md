# talon-task-limiter

C++17 有界任务执行器。保留 `ConcurrentTaskLimiter::Schedule` 的立即接纳语义，提供明确的关闭协议、move-only 任务支持，以及 `Submit` / `std::future` 异步结果。

```cpp
#include "talon/concurrent_task_limiter.h"

talon::ConcurrentTaskLimiter::Options options;
options.max_concurrent = 128;  // 正在构造 + 排队 + 执行/清理中的任务总数上限
options.worker_count = 8;      // 实际工作线程数；0 表示等于 max_concurrent
auto executor = talon::ConcurrentTaskLimiter::Create(options);
if (!executor.ok()) {
  // 处理初始化错误；不要访问失败的 StatusOr。
  return executor.status();
}

auto result = (*executor)->Submit([](int x) { return x * 2; }, 21);
if (!result.ok()) {
  return result.status();  // 满载或关闭；任务没有提交
}
int answer = result->get(); // 42；任务异常由 get() 重新抛出
return (*executor)->Shutdown();
```

完整可运行例子见 [examples/basic.cc](examples/basic.cc)。错误类型使用真实的 [talon-status](https://github.com/cq-cdy/talon-status)，没有仿造其接口，也不依赖 Abseil。

## 构建与测试

需要 CMake ≥ 3.20、C++17 编译器、标准线程库和 C++ 异常支持。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --parallel 4
```

依赖解析顺序是：已有 CMake target → 已安装 package → 固定提交的 FetchContent。GoogleTest 仅用于测试。首次下载需要网络；没有跟踪 `main` / `master` 的浮动依赖。

如果系统中有不兼容的 GoogleTest（例如 Conda 与系统 C++ 运行库混用），使用 `-DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON`，由当前工具链编译固定版本。离线源码覆盖示例：

```sh
cmake -S . -B build \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  -DFETCHCONTENT_SOURCE_DIR_TALON_STATUS=/path/to/talon-status \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
```

上述路径由使用者提供，项目没有任何对本机兄弟仓库的硬编码依赖。严格禁止下载时，设 `TALON_TASK_LIMITER_FETCH_DEPENDENCIES=OFF` 并预先提供所需 targets/packages。固定版本与许可证见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 第三方项目集成

源码方式默认不构建本库测试、示例或基准：

```cmake
add_subdirectory(external/talon-task-limiter)
target_link_libraries(my_app PRIVATE talon::task_limiter)
```

安装方式：

```sh
cmake --install build --prefix /your/prefix
```

```cmake
find_package(talon-task-limiter 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE talon::task_limiter)
```

配置消费工程时设置 `CMAKE_PREFIX_PATH`。从 FetchContent 源码构建的 talon-status 会随安装规则安装；若使用的是外部已安装依赖，消费端也需能找到它。包支持静态库和 `BUILD_SHARED_LIBS=ON`；共享库使用者仍须配置平台的动态库搜索路径。跨编译器、标准库或不兼容 CRT 的二进制混用不受支持。

## 接口与错误语义

| 接口 | 行为 |
| --- | --- |
| `ConcurrentTaskLimiter(int32_t)` | 启动同数量工作线程；必须大于 0 |
| `ConcurrentTaskLimiter(Options)` | 独立配置额度和线程数；构造失败抛异常，清理已启动线程 |
| `Create(Options)` | 返回 `StatusOr<unique_ptr<...>>`；配置错误为 `InvalidArgument`，资源/线程启动失败为 `ResourceExhausted` |
| `Schedule(func, args...)` | 立即尝试接纳，忽略任务返回值；返回 `Status` |
| `Submit(func, args...)` | 相同接纳协议；返回 `StatusOr<future<R>>`，支持 void、左值引用及 move-only 结果 |
| `Close()` | 幂等停止接纳新任务，已保留额度的任务仍会执行，不等待 |
| `WaitIdle()` | 等待观察到无任务；包含捕获对象析构，不关闭接纳 |
| `WaitIdleUntil(steady_clock::time_point)` | 同上；超时返回 `DeadlineExceeded`，不取消任务 |
| `Shutdown()` | Close + 排空 + join；幂等，支持多个外部线程同时调用 |
| `GetSnapshot()` | 同一锁下的一致快照；不保留额度 |

满载返回 `ResourceExhausted`；关闭返回 `FailedPrecondition`，关闭优先于满载。被拒绝时不复制或移动 callable/参数。这里的“立即”指不等待空闲额度；接口会竞争互斥锁，**不是 lock-free / wait-free，也不提供实时或公平性保证**。

成功取得额度后，空原始函数指针、成员指针以及空的原始/标准智能成员目标会返回 `InvalidArgument`，不会发布任务。自定义间接访问对象和借用引用仍须由调用者保证有效。future 的值结果要求可移动构造且析构不抛异常；void 和左值引用结果也受支持。

`Schedule` / `Submit` 中的任务分配、参数复制或移动异常原样传播，额度自动回滚。Status 的错误消息也可能分配内存，所以这些 API 不是完整内存耗尽场景下的 `noexcept` 接口。

构造阶段异常不承诺恢复已经移动的输入。拒绝时不移动的保证从函数入口开始，不覆盖调用表达式求值时已经发生的操作；需要重试 move-only 工作时，应像压力测试那样先保存具名 callable，再尝试提交。

### 异步异常

优先用 `Submit`，通过 `future.get()` 处理任务异常。`Schedule` 无 future；需要配置 `Options::on_unhandled_exception` 才能处理其任务异常：

```cpp
options.on_unhandled_exception = [](std::exception_ptr error) noexcept {
  // 在此记录或交给应用的错误处理设施；保证线程安全、不抛异常。
};
```

处理器运行在工作线程，可以并发调用。未配置处理器时，逃逸异常调用 `std::terminate()`；处理器再次抛异常也终止进程，不会悄悄忽略错误。`Submit` 的异常不调用此处理器。

### 生命周期与死锁边界

- 额度从接纳时开始，覆盖构造、排队、执行、异常回调及任务捕获对象析构。`future` 就绪可能早于额度释放；需要完整完成屏障时使用 `WaitIdle` 或 `Shutdown`。
- 参数遵循类似 `std::thread` 的 decay-copy、一次性右值调用规则；`std::ref` / `std::cref` 显式借用对象，借用对象必须活到任务及相关清理结束。返回的引用、view 或指针必须指向寿命足够长的外部对象，不能指向随任务清理的捕获对象。
- 所有常规接口可并发调用。外部所有者必须确保对象在调用期间有效，并在析构前结束所有外部调用；析构会排空内部任务。内部任务禁止销毁所属执行器，否则立即终止进程。
- 当前执行器的任务、异常处理器、捕获对象构造/析构回调调用其 `WaitIdle` / `Shutdown`，返回 `FailedPrecondition`，不改变接纳状态；调用 `Close` 和尝试 `Schedule` 是允许的。
- worker 的 `thread_local` 析构也受自等待保护；`Shutdown` 会等待线程退出清理，`WaitIdle` 只等待任务清理。用户保存到 TLS 中的引用必须保持相应寿命。
- 库不解析跨执行器或用户 future 的等待依赖。例如仅一个 worker 的任务等待该池另一个任务的 future，仍可能死锁。任务必须最终返回；没有强行终止线程、超时析构或抢占取消。

更精确的状态机、同步和关闭竞争解释见 [docs/design.md](docs/design.md)。

## 验证与性能

测试覆盖多提交者饱和、逐任务 exactly-once、关闭竞争、并发 join、空闲唤醒、move-only/过对齐类型、捕获异常、重入、future 生命周期及编译期约束。独立进程注入 `new` 失败，Linux 还注入真实 `pthread_create` 的 `EAGAIN`，验证部分启动回滚。

```sh
cmake --preset asan -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
cmake --build --preset asan --parallel 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --preset asan

# 完整 TSan（需要 clang-12/clang++-12；只安装到本地 build-toolchain）：
python3 tools/build_tsan_runtime.py --jobs 4
runtime_prefix="$PWD/build-toolchain/tsan-runtime"
cmake -S . -B build-tsan-full -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-12 -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON \
  "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -fsanitize=thread -fno-omit-frame-pointer -isystem $runtime_prefix/include/c++/v1" \
  "-DCMAKE_EXE_LINKER_FLAGS=-L $runtime_prefix/lib -Wl,-rpath,$runtime_prefix/lib" \
  -DTALON_TASK_LIMITER_SANITIZER=thread
cmake --build build-tsan-full --parallel 4
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan-full --output-on-failure
```

Sanitizer 构建不运行自定义 allocator / pthread interposition 测试，避免拦截机制冲突；这些测试在普通构建中单独运行。完整 sanitizer 配置仍验证安装消费。所有配置的实际执行状态、工具链和限制见 [docs/verification.md](docs/verification.md)。CI 配置本身不代表远端流水线已运行。

本机 libstdc++ 的 TSan 在 `std::call_once` 异常展开后存在独立复现的超时；未插桩 libc++ 又会漏掉 future 引用计数同步，产生可独立复现的报告。完整检查使用插桩后的 libc++/libc++abi，不添加 suppression 或跳过异常测试。

Google 格式与花括号检查：`python3 tools/check_style.py --build-dir build-debug`，配置时需开启 `CMAKE_EXPORT_COMPILE_COMMANDS`。异常边界和 C++17 兼容选择见 [风格说明](docs/style.md)，本轮确认并修复的问题见 [整体审查](docs/review.md)。

普通 `Schedule` 每个成功任务分配一个节点，发布队列不再分配；用户 callable 本身仍可能分配。`Submit` 另需 future 共享状态。线程复用、队列操作 O(1)，用户代码在状态锁外执行。吞吐基准及其适用范围见 [docs/performance.md](docs/performance.md)，不把 CAS 或微基准数字当作整体性能保证。

## 构建选项

| 变量 | 默认值 |
| --- | --- |
| `TALON_TASK_LIMITER_BUILD_TESTS` | 顶层 ON，作为子项目 OFF |
| `TALON_TASK_LIMITER_BUILD_EXAMPLES` | 顶层 ON，作为子项目 OFF |
| `TALON_TASK_LIMITER_BUILD_BENCHMARKS` | OFF |
| `TALON_TASK_LIMITER_FETCH_DEPENDENCIES` | ON |
| `TALON_TASK_LIMITER_STRICT_WARNINGS` | ON，仅作用于本项目 targets |
| `TALON_TASK_LIMITER_SANITIZER` | `none`；可选 `address`（ASan+UBSan）、`thread` |
| `BUILD_SHARED_LIBS` | 遵循 CMake / 父工程设置，未设置时静态库 |

本库最低 C++17，不强行降低父工程的语言标准。talon-status 的 `AUTO` 后端取决于语言模式；若应用的多个 TU 混用 C++17/C++23，应按该依赖的契约统一后端，例如使用 `COMPAT`。本库不通过二进制边界返回其可变后端的 `StatusOr`。

## 目录与许可证

```text
include/talon/       公共接口、模板任务节点
src/                线程、状态同步、队列和关闭实现
tests/              API、并发、生命周期、故障注入及外部消费
cmake/              固定依赖、包导出和集成验证
examples/           可运行示例
benchmarks/         可复现吞吐基准
docs/               设计契约、性能和验证记录
.github/workflows/  跨平台及 sanitizer CI 配置
```

许可证：[Apache-2.0](LICENSE)。这是 `0.1.0` 初始版本，尚不承诺未来版本的稳定二进制 ABI。
