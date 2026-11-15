# 状态传播宏与导出宏

## 状态传播

原先 `TryAcquire` 等调用后的显式检查可以直接使用 talon-status 的工具宏。本轮已将 `Create`、`Schedule`、`Submit` 的适用传播点替换为：

```cpp
TALON_RETURN_IF_ERROR(TryAcquire());
Reservation reservation(this);
```

带成功值的操作使用：

```cpp
TALON_ASSIGN_OR_RETURN(auto executor,
                       talon::ConcurrentTaskLimiter::Create(options));
TALON_ASSIGN_OR_RETURN(auto future, executor->Submit(task));
```

使用真实依赖的 `TALON_RETURN_IF_ERROR` / `TALON_ASSIGN_OR_RETURN`，没有重新实现同名工具。它们分别用于错误直接返回、检查 StatusOr 并移动取出值。宏表达式只求值一次；早返回仍执行正常 C++ RAII 析构。现有容量预留、错误码和消息传播语义保持不变。

库公共头不包含 `status_macros_short.h`。`RETURN_IF_ERROR` / `ASSIGN_OR_RETURN` 短名字是依赖提供的可选别名，调用端可以自行启用；库采用带前缀版本，避免覆盖调用者已有宏。

不是每一个 `if (!status.ok())` 都应替换：构造函数不能返回 Status；需要补偿、记录错误、完成 fan-in 或打印并转换退出码的分支，也应保留显式控制流。新的矩阵和 DAG 测试区分了“传播错误”和“记录分支失败后继续完成其他任务”。

## 公共模板中的 ODR 要求

talon-status 默认使用 `__COUNTER__` 为临时变量命名。公共 inline/模板定义若因翻译单元此前的宏展开次数不同而产生不同 token，会违反 ODR；普通链接成功不能证明没有这个问题。依赖的 [宏契约](https://github.com/cq-cdy/talon-status/blob/aaf333da1e5c9f494d5750f52fd0838aeb4e4205/docs/contracts.md) 建议这类定义统一使用行号模式。

本库现在要求：

```cpp
TALON_STATUS_USE_LINE_COUNTER=1
```

`talon::task_limiter` 通过 PUBLIC compile definition 将设置传递给构建和安装后的消费端。手动集成时，必须在任何 Talon 宏头之前定义它；公共头在未配置时采用 1，发现显式的 0 时给出编译错误。所有包含这些公共定义的翻译单元都应保持一致。

这是需要注意的编译配置变化：`TALON_ASSIGN_OR_RETURN` 每个物理行最多写一次，也不能用外层宏把多个 ASSIGN 折叠到同一展开行。已有代码若显式选择计数器模式，应改为此模式并按行书写。

`limiter.macro_odr` 实际预处理两份源文件，其中一份预先消耗 32 次 `__COUNTER__`，比较完整公共类定义；另有消费端短名冲突检查、错误传播/RAII 测试和配置冲突的预期编译失败检查。

## TALON_TASK_LIMITER_EXPORT 是什么

它是编译器的符号可见性标记，不是运行时函数，不参与任务调度，也不提供线程同步。

| 构建场景 | 典型展开 |
| --- | --- |
| 静态库 | 空 |
| 本机 GCC/Clang 共享库，构建端或调用端 | `__attribute__((visibility("default")))` |
| Windows 构建 DLL | `__declspec(dllexport)` |
| Windows 使用 DLL | `__declspec(dllimport)` |

本机静态/共享文件已直接检查；Windows 的映射依据 CMake 生成规则，未在本机执行 Windows 构建。[CMake GenerateExportHeader](https://cmake.org/cmake/help/latest/module/GenerateExportHeader.html)

例如 Linux 共享库中的：

```cpp
TALON_TASK_LIMITER_EXPORT Status Shutdown();
```

相当于：

```cpp
__attribute__((visibility("default"))) Status Shutdown();
```

项目默认隐藏实现符号，导出宏让需要跨库调用的函数可见。若直接删掉宏，共享库的调用端可能找不到 `Shutdown` 等符号。部分 private 辅助函数也需要导出：公开模板在调用者 TU 中实例化，会调用库内的 `TryAcquire`、`Enqueue`、Reservation 构造/析构；C++ 访问权限和动态链接可见性是不同的机制。

Reservation 只在构造/析构函数上标记导出，不能导出整个类。整个类的 DLL 标记会连带作用于静态 `thread_local current_`，MSVC 因此报 C2492。TLS 指针和 Contains 都只由库内部访问，不需要出现在 DLL 接口中。

生成规则位于 `CMakeLists.txt` 的 `generate_export_header`，头文件位于 `<build>/include/talon/task_limiter/export.h`，安装时复制到 `<prefix>/include/talon/task_limiter/export.h`。构建共享库时 CMake 自动定义 `talon_task_limiter_EXPORTS`，静态消费使用 `TALON_TASK_LIMITER_STATIC_DEFINE`；调用者链接公开 target 即可获得正确配置。
