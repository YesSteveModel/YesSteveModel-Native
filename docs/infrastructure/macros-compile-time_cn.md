# 宏与编译期设施

[返回基建总览](../infrastructure_cn.md)

主要头文件：`<log.h>`、`<profile.h>`、`<inline.h>`、`<fast_math.h>`、`<assume.h>`、`<cpu.h>`、`<export.h>`、`<macro.h>`。

| 类别 | 入口 | 契约 |
| --- | --- | --- |
| 日志 | `YSM_LOG`、`YSM_PLOG`、条件变体、`YSM_LOG_DEBUG/TRACE` | 使用 `std::format` 语法；被编译移除时参数不求值 |
| 日志作用域 | `LoggingScope` | thread-local 严格 LIFO；borrowed name 必须活到析构 |
| Profiling | `YSM_PROFILE_ZONE/VALUE`、zone/frame begin/end | 关闭时宏参数不求值；zone 按 token、frame 按名称配对 |
| 内联 | `YSM_INLINE`、`YSM_NOINLINE` | 用于确认过的热路径或分派边界 |
| 浮点优化 | `YSM_FAST_MATH_BEGIN/END` | 局部使用并保持 push/pop 配对 |
| 优化器假设 | `YSM_ASSUME` | 条件为假时是未定义行为 |
| ISA 目标 | `YSM_TARGET_AVX2/AVX512/SVE` | 仅从完成 feature dispatch 的路径调用 |
| 自旋与 fence | `ysm_pause`、`ysm_lfence/sfence/mfence` | 只用于已有内存序协议的底层代码 |
| 可见性 | `YSM_EXPORT` | 标记动态库导出；受开发指南的 ABI 约束 |
| 预处理器 | `YSM_PP_*`、`YSM_MACROS_CONCAT_NAME` | 供高层宏实现使用 |
| 字面量 | `YSM_SV_LITERAL` | 仅接受编译期字符数组 |

native runtime 初始化时调用一次 `InitializeLogging()`；之后可用 `ConfigureLogLevel()` 调整级别。`LoggingScope` 不可跨线程移动。

## 日志用法

severity 使用 `INFO`、`WARNING`、`ERROR` 或 `FATAL` token；`FATAL` 会终止进程。条件变体先传 severity，再传 condition，最后是格式串和参数：

```cpp
void LogDecodeResult(size_t size, bool recovered) {
    LoggingScope scope("Decode");
    YSM_LOG(INFO, "Decoded {} bytes", size);
    YSM_LOG_IF(WARNING, recovered, "Recovered {} bytes", size);
    YSM_LOG_DEBUG("Output size: {}", size);
}
```

`YSM_PLOG` / `YSM_PLOG_IF` 在同样形状的日志后附加 `errno` 信息。`LoggingScope` 根据构造参数推导模板类型：`std::string` 实例拥有名称；`string_view` / `char*` / `const char*` 实例借用名称，仅保存 view 和深度，不含 `std::string` 成员。作用域严格按 LIFO 析构，违反顺序会终止进程。

Debug/Trace 是否编入由 `YSM_ENABLE_DEBUG_LOG` 决定，未显式设置时按 `YSM_DEBUG` 默认；运行时 `ConfigureLogLevel` 只能控制已编入且未被 Abseil verbosity 上限裁掉的日志。项目构建选项会配置这些编译开关。关闭的 Debug/Trace 宏仍要求参数表达式可编译，但不会求值；不要将业务副作用放在日志参数中。

## Profiling 用法

普通同步作用域使用 scoped zone，`YSM_PROFILE_VALUE` 关联当前 zone：

```cpp
void ProfileBytes(size_t size) {
    YSM_PROFILE_ZONE("Decode");
    YSM_PROFILE_VALUE(size);
}
```

手动 zone 用 `SourceLocation` 开始、用返回 token 结束。下面额外包含 `<scope_guard.h>`，保证提前返回也会配对：

```cpp
void ProfileManualZone() {
    static const profile::SourceLocation location(
        "Decode", "ProfileManualZone", __FILE__, __LINE__);
    const auto token = YSM_PROFILE_ZONE_BEGIN(location);
    auto end_zone = ScopeGuard([token] {
        [[maybe_unused]] const bool ended = YSM_PROFILE_ZONE_END(token);
    });
}
```

`SourceLocation` 使用静态生命周期，以保活 profiler 引用的名称与源码信息。zone 在同线程内嵌套配对；token `0` 表示未激活，结束它是成功的空操作。`EndZone` 的 bool 只检查 token 形状，不检测重复结束或错误线程。

Frame 用固定名称配对，begin 返回是否实际开始；只有返回 true 才调用 end：

```cpp
void ProfileFrame() {
    static constexpr auto kFrameName = "ModelPreview";
    const bool started = YSM_PROFILE_FRAME_BEGIN(kFrameName);
    auto end_frame = ScopeGuard([started] {
        if (started) {
            YSM_PROFILE_FRAME_END(kFrameName);
        }
    });
}
```

`YSM_ENABLE_TRACY = 0` 时，zone begin 返回 `0`、zone end 返回 `true`、frame begin 返回 `false`，宏参数均不求值；宏外构造 `SourceLocation` 的表达式仍会执行。
