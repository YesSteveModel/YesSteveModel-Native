# 协作取消

[返回基建总览](../infrastructure.md)

主要头文件：`<cancellation.h>`。状态属于 `ysm-core`，不依赖 JNI、存储、渲染或调度。

## 状态与可见性

`CancellationState` 新建为未请求。`Request()` 以 release 发布已取消状态，`IsRequested()` 以 acquire 读取。状态没有 reset；重复和并发请求幂等，请求完成后开始的检查可见取消，与请求重叠的检查可见任一状态。多个消费者可检查同一状态，独立状态互不影响。

状态只表达请求。消费者选择检查点，并拥有结果、终止、恢复及资源释放；取消不提供强制中断或固定响应时延。

## 所有权与 JNI

同步调用可借用 `CancellationState`，借用方应保证状态存活。跨同步窗口的消费者持有自己的共享保留；`shared_ptr` 保活状态，不拥有消费者或工作终态。

`NativeCancellation` 继承 `NativeObject`，创建时采用 typed opaque handle。Java 使用 `token.borrow(handle -> nativeCall(handle))` 传递 handle；借用回调不持有执行锁，`request()` 自身也是短借用。回调不能保留原 handle，不能调用或等待同一 wrapper 的 `close()`。继承的 `get()` 不建立借用窗口。

Entry 只在有效借用内调用 `AcquireOpaquePtr<CancellationState>`。创建使用 `nCreate()J`，失败为 `0`；请求使用 `nRequest(J)Z`，失败为 `false`；查询使用 `nQuery(J)I`，值为未请求 `0`、已请求 `1`、失败 `-1`。Java 显式报告失败，不将失败当成未请求。Entry 只用 primitive 载体，不读取 Java 状态或回调 Java 方法。

Java `close()` 先拒绝新借用，再等待已有借用结束，最后通过基类释放一次 handle。关闭不隐含取消，可重复调用；等待中断在释放后恢复。活动借用保活 wrapper，Cleaner 不能提前释放；创建包装或注册 Cleaner 失败由 adoption 回收。原 handle 释放后，已取得独立保留的 native 消费者仍可检查状态，最后保留结束后销毁 backing。

## 验证入口

`CancellationTest` 使用非数据库消费者，验证单向状态、可见性、多消费者与最后共享保留的物理释放。`NativeCancellationIntegrationTest` 使用生产 JNI，验证请求并行、封口与等待、异常归还、Cleaner、失败哨兵及 adoption 回滚。
