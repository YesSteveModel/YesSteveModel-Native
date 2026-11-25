# 通用模板与 RAII

[返回基建总览](../infrastructure.md)

主要头文件：`<scope_guard.h>`、`<non_copyable.h>`、`<move_only.h>`、`<empty.h>`、`<enum.h>`、`<bitpack.h>`、`<hierarchy/path_walker.h>`、`<hierarchy/tree.h>`。

## 生命周期工具

| 入口 | 契约 |
| --- | --- |
| `ScopeGuard<F>` | 作用域退出时执行 callable；`Release()` 取消；析构回调不得抛异常 |
| `NonCopyable` | 禁用复制，不自动提供 move |
| `YSM_MOVE_ONLY(Type)` | 默认 move、删除 copy；要求成员的默认 move 语义正确 |
| `EmptyOr<condition, T>` / `YSM_EMPTY_OR` | 按编译期条件存储 `T` 或空类型 |

```cpp
auto guard = ScopeGuard([&] { ReleaseResource(resource); });
YSM_RETURN_IF_ERROR(UseResource(resource));
guard.Release();
```

## 枚举分派

- `EnumCast<E>(raw)`：合法值转为 `E`，否则返回 `InvalidArgument`。
- `EnumValidate(values...)`：校验多个 scoped enum。
- `EnumSwitch(fn, values...)`：把运行时 enum/bool 组合转为 compile-time tag 组合。

`EnumSwitch` 将每个参数转换为一个带 `value` 的 compile-time tag，callback 必须能实例化所有组合且返回类型兼容。分派前用 `EnumCast` / `EnumValidate` 校验外部值；`EnumSwitch` 本身不提供 status 错误传播。

```cpp
enum class SampleKind : uint8_t { kByte, kFloat };

absl::StatusOr<size_t> SampleWidth(uint8_t raw_kind, bool stereo) {
    YSM_DECLARE_OR_RETURN(kind, EnumCast<SampleKind>(raw_kind));
    return EnumSwitch([](auto kind_tag, auto stereo_tag) -> size_t {
        constexpr auto kind = decltype(kind_tag)::value;
        constexpr auto channels = decltype(stereo_tag)::value ? 2 : 1;
        if constexpr (kind == SampleKind::kFloat) {
            return channels * sizeof(float);
        } else {
            return channels * sizeof(uint8_t);
        }
    }, kind, stereo);
}
```

此例还需 `<err.h>`。组合数量随参数取值数相乘增长，适合已有有限模板特化。

## 位打包

`YSM_BIT_PACK` 按声明顺序从最低位生成 `pack` / `unpack`：

```cpp
enum class PacketKind : uint8_t { kData = 1, kEnd = 2 };

YSM_BIT_PACK(PacketFlags, uint32_t,
             YSM_BIT_FIELD(size, uint32_t, 24),
             YSM_BIT_FIELD(kind, PacketKind, 8));

absl::StatusOr<uint32_t> EncodeFlags(uint32_t size, PacketKind kind) {
    YSM_DECLARE_OR_RETURN(raw, PacketFlags::pack(size, kind));
    return raw;
}

absl::StatusOr<PacketKind> DecodeKind(uint32_t raw) {
    const auto [size, kind] = PacketFlags::unpack(raw);
    YSM_BIT_ENUM_VALIDATE(kind);
    return kind.Value();
}
```

`pack` 返回 `StatusOr<storage>`，字段越界或枚举非法返回 `InvalidArgument`，不会静默截断。`unpack` 返回 tuple；有符号字段按声明位宽符号扩展，枚举字段返回 `EnumValue<E>`。对非法枚举直接调用 `Value()` 或隐式转换会抛 `std::invalid_argument`；先用 `Valid()` / `Status()` / `YSM_BIT_ENUM_VALIDATE` 校验，或用 `ValueOr(fallback)` 明确选择替代值。

## 路径层级树

`hierarchy::PathWalker` 按 `/` 遍历规范化相对路径。`hierarchy::Tree<T>` 存储 `std::string_view` key 和 `T` 的引用；两者都必须比 tree 存活更久。
