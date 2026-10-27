# JNI 基建

[返回基建总览](../infrastructure_cn.md)

主要头文件：`<java/ref.h>`、`<java/class.h>`、`<java/string.h>`、`<java/array.h>`、`<java/buffer.h>`、`<java/opaque_ptr.h>`、`<java/entry.h>`。

JNI 边界依赖 `ysm-java`。

## 引用、字符串与 class

| 入口 | 契约 |
| --- | --- |
| `Ref<T>` / `LocalRef(env, obj)` | RAII local reference；绑定创建它的 `JNIEnv_*`，不得跨线程或 native frame 保存 |
| `DummyRef(obj)` | 不删除引用，生命周期仍由调用方管理 |
| `FindClass` | 使用项目 class lookup 并返回 status |
| `U8ToStr` / `StrToU8` / `StrToPath` / `PathToStr` | Java 字符串与 native UTF-8/path 转换 |

项目没有通用 global-reference wrapper；跨线程或 frame 持有 Java 对象需要专用 owner。

## 数组与 buffer

`ObjectArray` 创建对象数组。`Read*Array`、`Write*Array` 和 `*Array` 覆盖 JNI primitive array，并校验 offset/range。

- 默认 `kSafe = false` 可使用 `GetPrimitiveArrayCritical`；pin 期间不得调用 JNI 或阻塞。
- `kSafe = true` 使用 region API 和 native 临时副本。
- `Critical*Array<kReadOnly>` / `CriticalBuffer<kReadOnly>` 是 move-only RAII；只读实例以 `JNI_ABORT` 释放。

`BufferInput<kReadOnly, kSafe>` 统一读取 `byte[]` 与 direct buffer。Java 用 `BufferArgument.packInput()` 提供对象和 flags；native 测试可用 `BufferInputFlags::pack(size, offset, type)`，并处理其 status。`Get` 的可选 `required` 参数检查最小长度，不会把 view 截断成该长度。

| 输入 | `kSafe` | 存储与写入效果 |
| --- | --- | --- |
| `byte[]` | `false` | 持有 critical array；`kReadOnly = false` 在释放时提交修改，`true` 以 `JNI_ABORT` 释放且调用方不得写入 |
| `byte[]` | `true` | 复制到 native cache；即使 `kReadOnly = false` 也没有自动回写，不能用于原地修改 Java 数组 |
| direct buffer | 任意 | 直接借用原 backing；可写使用会立即修改原内存，`kSafe` 不增加副本或生命周期保障 |

`kReadOnly` 是调用方必须遵守的访问契约，不能假定所有 view 转换都强制只读。需要 safe 数组写回时显式调用 `WriteByteArray<true>`；direct 输入使用期间原始 Java owner 必须保持可达。

`BufferOutput<kSafe>::Create()` 创建固定长度输出，填满有效范围后用 `release()` 取得 Java 对象。`TryMoveToOutput(env, std::move(buffer), type)` 接管 native buffer：array 输出复制 bytes，direct 输出成功后移交 allocation；`CopyToOutput` 保留输入所有权。只有配套 `NativeHeapBuffer` owner/cleaner 的 API 才能接收 owning direct 输出。

`release()` 和输出 helper 的 JNI 对象创建可能返回 `nullptr`，`StatusOr<jobject>::ok()` 不能替代非空检查；pending Java exception 仍由 JVM 传播。`TryMoveToOutput` 的 direct 分支在非空且没有 pending exception 后才移交 allocation，失败时仍由 RAII 释放。Java 用 `NativeHeapBuffer.adopt` 检查并立即接管 owning 输出，wrapper 分配和 Cleaner 注册失败也释放同一 allocation。

下面是配套的复制入口示例；安全输入模式允许在复制期间使用一般 native 能力，且在创建 Java 输出前结束输入作用域。

```cpp
#include <java/buffer.h>
#include <java/entry.h>

namespace ysm {
YSM_JNI_ENTRY(
    "Lcom/example/NativeCopyExample;nCopy(Ljava/lang/Object;J)Ljava/lang/Object;",
    (source, flags)) {
    BufferManaged output;
    {
        YSM_DECLARE_OR_RETURN(
            input, (java::BufferInput<true, true>::Get(env, source, flags)));
        output.CopyFrom(input);
    }
    return java::TryMoveToOutput(
        env, std::move(output), java::BufferType::kDirect);
}
}  // namespace ysm
```

```java
package com.example;

import com.elfmcys.ysm.buffer.UniBuffer;
import com.elfmcys.ysm.buffer.annotation.Owned;
import com.elfmcys.ysm.natives.buffer.BufferArgument;
import java.lang.ref.Reference;

public final class NativeCopyExample {
    @Owned
    public static UniBuffer copy(UniBuffer source) {
        try {
            var input = BufferArgument.packInput(source);
            var output = nCopy(input.obj(), input.flags());
            if (output == null) {
                throw new IllegalStateException("Native copy failed");
            }
            return BufferArgument.unpackOutput(output);
        } finally {
            Reference.reachabilityFence(source);
        }
    }

    private static native Object nCopy(Object source, long flags);
}
```

示例假定 native 库已加载、entry 已绑定；`unpackOutput` 只能接管配套入口返回的 owning base buffer，不能用于任意 direct slice。

## Native handle

`MakeOpaquePtr<T>` 将 `shared_ptr<T>` 放入 `jlong` handle；`ShareOpaquePtr` 复制共享所有权；`CastOpaquePtr<T>` 返回其中的引用；`AcquireOpaquePtr<T>` 返回一份 shared pointer。

每个 create/share 必须对应一次 `DestroyOpaquePtr`。异步工作在 handle 销毁前调用 `AcquireOpaquePtr`；handle 不提供 `T` 的线程安全性。

`CastOpaquePtr<T>` 的结果是 `StatusOr<reference_wrapper<shared_ptr<T>>>`，用 `YSM_DECLARE_OR_RETURN` 解包后借用 handle 内的 shared pointer；显式拷贝或 `AcquireOpaquePtr<T>` 才取得独立副本。

## Native entry 注册

```cpp
YSM_JNI_ENTRY(
    "Lcom/example/NativeApi;nSize(Ljava/lang/Object;J)J",
    (input, flags), jlong{-1}) {
    YSM_DECLARE_OR_RETURN(
        buffer, java::BufferInput<true>::Get(env, input, flags));
    return static_cast<jlong>(buffer.size());
}
```

- descriptor 使用 Minecraft 模组 Mixin 的 method desc 格式：`L<class>;nMethod(args)return`。方法名以 `n` 开头；参数数量和 JNI 类型在编译期校验。
- `V` / `Z` 实现返回 `absl::Status`；其他返回类型使用 `absl::StatusOr<JNI type>`。
- status 或异常由 entry 转为日志和 fallback，不会自动抛出 Java exception。
- entry 由 registrar 收集，并在 `BindEntry` 阶段统一注册。

设备分类同样通过 `NativeRuntime` 与 `YSM_JNI_ENTRY` 进入，不在配置类建立 native 旁路。其 `jlong` 输入为最大堆 byte 值，失败 fallback 由 Java 门面解释；Java 缓存一次结果。编码与默认责任唯一见[设备配置默认](../../../YesSteveModel/docs/architecture/implementation/native-runtime/overview.md#设备配置默认)。
