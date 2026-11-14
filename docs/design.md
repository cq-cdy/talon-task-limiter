# 设计与并发契约

## 范围

执行器拥有固定工作线程和有界任务队列。上限定义沿用原始实现：限制的是已取得额度但未完成清理的任务总数，而不是仅限制 CPU 上正在执行的任务数。默认线程数等于额度；可用 `Options::worker_count` 减少线程数并允许有界排队。

提供即时接纳、future 结果、停止接纳、空闲等待和排空关闭。动态扩缩容、优先级、外部执行器、强制取消和隐式重试没有混入核心协议。

## 状态与线性化点

一个状态互斥锁保护 `accepting`、队列头尾、`outstanding`、`queued`、`running`。任意持锁观察均满足：

```text
0 <= running <= worker_count <= max_concurrent
0 <= queued + running <= outstanding <= max_concurrent
constructing = outstanding - queued - running
```

| 操作 | 状态转换 / 线性化点 |
| --- | --- |
| 接纳 | 持锁检查 accepting 和 outstanding，成功时 outstanding++ |
| 拒绝 | 同一锁下观察到 closed 或 full；解锁后构造错误消息 |
| 发布 | 锁外构造完成后持锁将节点链接到队尾，queued++ |
| 开始执行 | 持锁出队，queued--、running++ |
| 完成 | 锁外完成调用、异常处理与捕获析构后，持锁 running--、outstanding-- |
| 构造回滚 | Reservation 析构时持锁 outstanding-- |
| Close | 持锁 accepting=false；这是不可逆状态转换 |
| WaitIdle 成功 | 持锁观察 outstanding==0；后续仍可能有新任务接纳 |
| Shutdown 成功 | Close 后所有 worker 已 join；无任何未完成的接纳 |

队列顺序是**发布顺序**。多个提交者的构造耗时不同，所以不承诺调用顺序或接纳顺序 FIFO；多个 worker 的开始/完成顺序也不保证。单 worker 的队列执行按发布 FIFO。

## 为什么没有单独的原子 CAS 计数器

原片段中的 CAS 只解决上限预留，不能自行处理队列发布、关闭状态和 idle 条件变量的丢失唤醒。这里用一个状态锁组成原子协议，简化证明和故障路径；避免 CAS 计数与队列锁之间的第二套状态同步。没有宣称整个调度过程无锁。

每次提交只在短临界区接纳和发布。节点由提交线程在锁外构造，内嵌 next 指针，入队没有容器扩容。worker 持锁只移交节点和更新状态。任务调用、回调、函数返回值和捕获对象的析构均在状态锁外进行。

## 关闭时尚未发布的任务

线程 A 成功接纳，正在执行用户 callable 的复制构造；线程 B 调用 Close。A 的额度已经预留，因此 Close 后仍允许 A 发布任务，或者在构造异常时回滚。

worker 的等待谓词是：

```text
queue 非空 || (!accepting && outstanding == 0)
```

不能仅在 `!accepting && queue.empty()` 时退出，否则构造中的任务会永远无人处理。最后一次完成或回滚将同时通知 idle 等待者和准备退出的 worker。谓词与通知所依据的状态使用同一个互斥锁，防止丢失唤醒。

Close 后，某个已成功接纳的 Schedule 调用仍可能返回 OK。Shutdown 会等待它的任务清理完成，但调用者仍须独立管理外部 Schedule 调用本身的生命周期；Shutdown 不是对象引用计数机制。

## 资源所有权与异常

构造成功前，线程数组属于正在构造的 Impl；任何线程创建异常均先 Close 并 join 已启动线程，再重新抛出。任务节点的所有权依次为提交线程、队列、worker，每步都是 unique_ptr 转移；节点从未同时归两个所有者所有。

Reservation 管理接纳失败回滚。任务成功发布后 worker 管理完成释放，因此没有将 guard 原始指针带入未知外部线程池的生命周期问题。节点不能复制，内部虚函数允许 move-only callable；没有将其强行包装成要求可复制目标的 `std::function<void()>`。

`Submit` 使用节点内的 `std::promise<R>`。callable/参数由节点直接持有，future 只持有结果共享状态。这刻意避免可调用对象随 packaged_task 共享状态一直存活到 future 销毁的实现差异。异常存入 future，设置返回值时的移动异常同样存入 future。结果对象、异常对象或返回引用的寿命与捕获对象的寿命是不同契约。

用户析构函数必须不抛异常。Schedule 的异步异常必须进入用户配置的处理器或终止进程；不把异常计数后继续运行当成错误处理。

## 重入、锁顺序与对象析构

线程局部变量记录当前 worker 所属执行器。提交期间的 Reservation 使用线程局部链表跟踪嵌套接纳，覆盖“捕获构造函数递归提交到另一个执行器，再等待外层执行器”的自等待情形。

worker 身份在循环返回后也保留到线程退出，以覆盖用户 `thread_local` 析构回调。对象在 join 完成前保持存活；该 TLS 指针是平凡对象，不需要主动清零。`WaitIdle` 的屏障不包含线程局部析构，`Shutdown` 的 join 包含这一阶段。

`WaitIdle` / `Shutdown` 在取得 join 锁前检查当前执行器是否出现在这些上下文中。拒绝自身等待不会关闭执行器。任务捕获析构时，worker 标记和额度仍然有效。

join 锁只由外部 Shutdown 调用者持有，串行化 `std::thread::join`；这些调用者可能再短暂取得状态锁。worker 永远不取得 join 锁，状态锁不会跨越 join 或用户代码。不存在内部反向获取顺序。互斥锁解锁/加锁以及最终 join 建立任务写入的可见性。

析构从普通所有者线程关闭、排空和 join。内部任务或提交回调销毁所属执行器将 `std::terminate()`；既不能 join 自己，也不能在 this 被销毁后继续执行成员函数。外部线程须在开始析构前结束所有对对象的调用。异常处理器及其捕获对象须活到最后一次回调结束；其自身在执行器销毁时的析构不得再次访问已在销毁的执行器。

库只能检测直接的自身等待，无法证明任意用户依赖图无环。例如池 A 与池 B 的任务互相等待，或任务持有外部锁而销毁者拿该锁等待 Shutdown，都由应用负责避免。用户任务永不返回时，Shutdown/析构也不会完成。

## 接口兼容性

与给出的片段相比，`Schedule` 的名称、完美转发、满载拒绝、不可复制移动和构造方式保持一致。返回类型从 `absl::Status` 改为 `talon::Status`，放在 `talon` 命名空间。没有依赖来源不明的 `ThreadPool`；线程资源由库自身拥有。

公共非模板实现经 PImpl 隔离；模板节点放在标记为 internal 的头中。`StatusOr` 工厂在调用端内联构造，降低可变后端跨动态库边界的风险。应用中 talon-status 的后端和 ABI 配置仍须一致；不保证任意版本或不同 CRT 的 ABI 兼容。

本轮对空原始 callable/member pointer、空标准成员目标增加运行时拒绝；future 的值结果增加可移动和 noexcept 析构约束。可空输入校验仍在满载/关闭判断之后。普通错误走 Status，原构造接口和标准库/用户异常清理边界保留兼容行为，详见 [style.md](style.md)。
