# CPU、SIMD 与并行执行

[返回基建总览](../infrastructure.md)

主要头文件：`<cpu.h>`、`<cpu_topology.h>`（`core`）；`gfx/src/renderer/parallel_executor.h`（`gfx` 内部实现）。

使用 `simd::kSupported` 或 runtime ISA 分派前调用 `InitCpuInfo()`。`simd::Tag<Type>` 及 `width/bytes/ints/floats` 向模板实现传递编译期宽度；`simd::Mock()` 仅供测试。

`DetectCpuTopology()` 是 best-effort 查询，失败返回 `std::nullopt`。性能核绑定同样允许失败，不得成为启动前置条件。
