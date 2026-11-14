# 风格与异常边界

本项目采用 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) 的格式、命名、封装、头文件和可读性规则，并额外要求所有控制语句保留花括号。普通条件分支、循环、测试辅助代码和示例均适用；第三方源码不做批量风格改写。

执行检查：

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
python3 tools/check_style.py --build-dir build-debug
```

脚本只检查，不修改文件。使用 clang-format 12 的 Google preset 和 clang-tidy 12 的 Google / braces 检查；代码中的逻辑类使用 private 数据成员和尾下划线，纯配置/快照仍为 struct。不使用 `using namespace`。两个测试中的隐式转换运算符有局部、带原因的 lint 豁免，用于验证外部用户类型的转换语义，生产代码没有该豁免。

这不等于逐条套用 Google 内部平台政策：当前指南针对 C++20，本库保留已经公布的 C++17 最低版本。Google 的异常禁用规则也不能原样套用到当前依赖和兼容接口；差异明确列在下面，不宣称支持 `-fno-exceptions`。

## 普通错误使用 Status

非法配置先在 `Create` 中校验并返回 `InvalidArgument`，不再通过抛出/捕获 `invalid_argument` 实现这一控制流程。额度耗尽、关闭、自等待、空原始函数/成员指针及空标准成员目标、等待超时同样直接返回状态。基准的命令行输入错误使用状态或退出码。

业务任务可以返回 `Status` / `StatusOr<T>`，通过 `Submit` 的 future 获取，无须主动抛出业务异常。接纳成功和业务执行成功是两件事：外层 `StatusOr` 表示接纳，future 中的值表示业务结果。

## 保留的边界及原因

| 位置 | 保留原因 |
| --- | --- |
| 原有公开构造函数 | 保持已公布的失败语义；新代码优先使用 `Create` |
| 部分线程启动失败的 catch/rethrow | 必须先 join 已启动线程，再传播标准库资源错误 |
| 任务和异常处理器边界 | 保持 future 的异常传播及 Schedule 的明确失败策略；不能悄悄吞错 |
| 分配、标准库、用户复制/移动代码 | 这些操作可能抛异常；用 RAII 保证回滚，不伪造 noexcept |
| 测试故障注入 | 有意抛出异常，验证真实异常展开和资源清理 |

固定的 talon-status 在 `include/talon/config.h` 中明确要求异常支持，其 `StatusOr` 的错误访问也有异常语义。完全禁用异常需要重新设计公开构造方式、异步结果和依赖策略，不能靠删除 catch 或添加 `noexcept` 完成。

## 静态分析与测试代码

风格检查应用于所有可正常编译的本项目翻译单元；预期编译失败的源文件检查格式，并由独立的负向编译测试验证诊断。

生产实现还单独运行 use-after-move、异常逃逸、悬空句柄和 Clang Static Analyzer 检查。故障注入/死亡测试有意构造抛异常、移动后检查和自身销毁路径，不能将这些预期路径的静态诊断与生产缺陷混为一谈。宽范围分析的诊断和实际验证边界在 [验证记录](verification.md) 中列明；没有通过移除运行时测试来取得静态检查通过。
