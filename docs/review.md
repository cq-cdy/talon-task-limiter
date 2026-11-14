# 整体审查与修复

审查基线为 `31e876d`。范围包括公开模板、任务节点、接纳/发布/回滚状态、线程启动与退出、析构重入、future 所有权、安装导出、sanitizer 消费、测试可靠性及编译诊断。以下问题均有失败证据和修复验证。

## 已修复的问题

| 严重程度 / 置信度 | 位置 | 触发与影响 | 证据及修复 |
| --- | --- | --- | --- |
| P1 / 高 | `src/concurrent_task_limiter.cc`，`Impl::WorkerLoop` 的退出路径 | worker 清除身份后，用户 thread_local 析构调用同池 Shutdown；外部 join 与内部 join 锁相互等待 | 回归在修复前由 `timeout 5s` 终止，退出 124；身份标记现在持续到线程真正退出，回归通过 |
| P1 / 高 | `include/talon/concurrent_task_limiter.h`，Schedule/Submit | 空原始函数/成员指针或空标准成员目标被派发，可能在 worker 触发未定义行为 | 空函数指针回归先得到错误的 OK，随后段错误退出 139；现在返回 InvalidArgument，不发布任务并归还额度 |
| P2 / 高 | `CMakeLists.txt`，sanitizer link options | instrumented 静态库被普通消费端链接时缺少运行库 | 外部项目实际出现 `__asan_*` / `__ubsan_*` 未定义引用；运行库链接参数改为 PUBLIC，并加入裸消费端及安装消费检查 |
| P2 / 高 | `cmake/RunConsumer.cmake`，参数转发 | CMAKE_PREFIX_PATH 的分号列表被拆成多个命令行参数 | 新增列表长度检查后配置失败，并出现忽略额外路径的警告；现在在两层转发中保留列表和空格 |
| P2 / 高 | `cmake/RunConsumer.cmake`，清理前检查 | 参数缺失或构建目录错误时，脚本先清理目录，再报配置错误 | 临时目录中的受控哨兵在旧脚本下被删除；现在先校验参数和构建标识，同样错误输入会保留哨兵 |

相关回归：`Validation.*`、`Invocation.Null*`、`Lifecycle.ThreadLocalDestructorsCannotSelfJoinAtWorkerExit`、`Lifecycle.ShutdownIncludesThreadLocalDestructionButIdleDoesNot`、`limiter.sanitizer_consumer`、`limiter.package`。

## 接口与实现整理

- `Create` 对非法选项直接返回状态；原构造接口保留兼容的异常行为。
- 空输入校验发生在取得额度之后、捕获/发布之前，因此既有的关闭/满载优先级保持不变；错误消息构造失败也由 Reservation 回滚。
- future 的值类型必须可移动构造并有 noexcept 析构，void 和左值引用单独允许。原来可能在 future 销毁时终止进程的类型现在得到明确编译诊断。这是有意收紧的编译期约束。
- 实现类和任务队列节点改为明确封装的 class，成员统一命名；所有控制语句补齐花括号。
- 分配故障注入拆成独立翻译单元。Release 的 GCC 会把同一 TU 中内联的替换 delete/free 误判为 mismatched-new-delete；拆分保留了严格警告和全部测试，并禁止该测试目标的全局 LTO 合并。

## 同步与生命周期结论

状态锁统一保护接纳、队列和计数。join 锁只由外部 Shutdown 调用者取得；worker 自等待检查在取得 join 锁之前完成。用户调用、参数构造、异常处理及捕获析构均不在状态锁内。已预留但未发布的任务始终计入 outstanding，关闭不会让 worker 提前退出。

任务节点只通过 unique_ptr 在提交者、队列和 worker 之间转移；出队时 next 被移出，销毁当前节点不会递归销毁仍排队的节点。额度在捕获清理结束后释放。线程局部资源的析构属于线程退出阶段，`Shutdown` 等待它，`WaitIdle` 不等待它。

worker 身份指针本身是平凡 TLS 对象，在开始执行用户代码前设置；执行器在 join 完成前仍然存活，因此线程退出回调可以安全识别自身上下文。下一条 OS 线程会使用自己的 TLS 存储，没有跨线程复用身份。

## 更强的验证

新增测试不是仅增加循环次数：

- 36 组历史参数、每组 8 个独立并发历史；根据调用区间搜索符合实时约束的接纳/关闭/快照串行解释，检查器本身也用不合法历史校验。
- 48 组预留矩阵：Schedule/Submit、开放/关闭、全部成功/部分失败/全部回滚、不同队列与 worker 数；逆序解除构造门闩，并发等待者必须等最后的预留处理完。
- 最深 128 层构造重入与异常展开，检查所有额度和 TLS reservation 恢复。
- 三个执行器上的树形任务图，最多 3,280 个节点，逐节点核对执行和资源销毁。
- 64 个提交线程处理 262,144 个 move-only 任务，持续满载重试时验证拒绝不转移所有权，最终逐任务核对执行一次与清理一次。
- 参数转换、成员指针、引用身份、over-alignment、共享 future、结果/错误寿命和完整分配/线程启动故障注入。
- 16 项预期编译失败，先通过正向编译链接基线，再匹配具体失败原因。

实际运行结果见 [验证记录](verification.md)，性能和编译成本对照见 [性能记录](performance.md)。

## 尚需调用者满足的契约

不能把测试通过解释为任意用户代码都无死锁或无 crash。外部调用期间对象必须存活；借用对象以及用户在 TLS 中保存的引用必须满足各自寿命；用户共享数据、复制/移动/析构回调和异常处理器需要自行同步。任务必须最终返回。

`Schedule` 的未处理异常仍按已公布策略终止进程；使用 `Submit` 或提供不抛异常的处理器。任务在同池 future 上阻塞、多个执行器循环等待、调用者持外部锁再等待 Shutdown 等依赖死锁不由标准 future 接口自动解决。源码兼容的 C++17 和异常边界选择见 [风格说明](style.md)。

没有发现需额外改变核心线程模型或队列算法的证据，也没有宣称任何负载下的全局最优性能。性能数据保留了不利样本；在明确业务负载之前，不为微基准引入第二套同步状态或更复杂的无锁协议。
