# 架构总览

## 模块依赖拓扑

所有生产模块均为静态库，最终仅由 `lib-jni` 聚合导出单一共享库 `ysm`。

```mermaid
flowchart LR
    core["core<br/>buffer / allocator / cpu / log / cancel"]
    proto["proto<br/>模型与资产 Schema 值类型"]
    model["model<br/>模型骨序"]
    codec["codec<br/>hash / zstd / archive / image / opus"]
    gfx["gfx<br/>bake / pose / schedule / CPU render"]
    physics["physics<br/>共享碰撞形状 / 每实体 Jolt 世界"]
    storage["storage<br/>SQLite 业务索引与协调"]
    legacy["legacy<br/>旧版格式解码与转换"]
    java["java<br/>JNI 载体 / opaque handle / entry 注册"]
    jni["lib-jni (SHARED: ysm)<br/>JNI 胶合与唯一导出边界"]

    core --> codec
    core --> gfx
    core --> physics
    core --> storage
    core --> legacy
    core --> java
    proto --> gfx
    proto --> model
    proto --> physics
    proto --> legacy
    model --> gfx
    model --> physics
    codec --> legacy

    core --> jni
    codec --> jni
    gfx --> jni
    physics --> jni
    storage --> jni
    legacy --> jni
    java --> jni
```

第三方依赖通过 Conan 静态链接并收窄到最小消费 target。

## 平台与 ISA 基线

| target | ISA | OS / runtime |
| --- | --- | --- |
| Windows x86_64 | [`x86-64-v1`](https://clang.llvm.org/docs/UsersManual.html#x86) | Windows 7 SP1 |
| Linux x86_64 | [`x86-64-v1`](https://clang.llvm.org/docs/UsersManual.html#x86) | Linux 3.13.9 + glibc 2.19 |
| Android arm64 | `armv8-a` | Android API 28 |
| macOS arm64 | Apple M1（接近 `armv8.5-a`） | macOS 14.5 |

更高指令集（x64：`SSE41` / `AVX2` / `AVX512` 等；arm64：`SVE`）仅允许出现在带目标属性隔离的模板/函数实现中，启动时经 `InitCpuInfo()` 探测并写入 `simd::kSupported`，调用期经 `simd::Tag<Type>` 与 `EnumSwitch` 分派。

## 文档索引

- [开发指南](development_guide.md)
- [构建指南](build.md)
- [构建系统](build-system.md)
- [依赖管理](dependencies.md)
- [新平台适配](platform-porting.md)
