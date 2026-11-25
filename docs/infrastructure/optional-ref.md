# `gch::optional_ref` 可空借用

[返回基建总览](../infrastructure.md)

头文件：`<optional_ref.hpp>`。CMake target：`optional_ref`。

`gch::optional_ref<T>` 表示“可能不存在的 `T` 借用”。它只保存指针，不拥有对象、不延长生命周期，也不分配内存。

## 类型与生命周期

- 可修改借用使用 `gch::optional_ref<T>`；只读借用使用 `gch::optional_cref<T>`，即 `gch::optional_ref<const T>`。
- 被引用对象必须覆盖 `optional_ref` 的全部使用期。不要返回对局部对象的引用，也不要把指向容器元素的 `optional_ref` 保留到可能使该元素引用失效的操作之后。
- `optional_ref` 的复制、移动、赋值、`reset()` 和 `emplace()` 只复制或替换借用，不复制、移动或销毁被引用对象。
- wrapper 自身的 `const` 不会使目标只读：`const gch::optional_ref<T>` 仍返回 `T&`。需要只读契约时必须使用 `gch::optional_cref<T>`。

默认构造、`gch::nullopt` 和空指针都会产生空值。优先从 lvalue 显式构造；从指针构造适合已有可空指针的适配边界。

```cpp
gch::optional_ref<Item> FindItem(ItemMap& items,
                                 std::string_view name) noexcept {
    auto iter = items.find(name);
    if (iter == items.end()) {
        return gch::nullopt;
    }
    return gch::optional_ref<Item>{iter->second};
}

gch::optional_cref<Item> FindItem(const ItemMap& items,
                                  std::string_view name) noexcept;
```

## 访问

先用显式 bool 转换或 `has_value()` 检查，再通过 `*` 或 `->` 访问：

```cpp
if (auto item = FindItem(items, name)) {
    item->Update();
}
```

- 空值上的 `*` / `->` 是未检查访问，不得使用。
- `value()` 在空值上抛出 `gch::bad_optional_access`。不要用它表达正常的缺失分支或热点路径控制流；只有已经建立非空不变量时才使用。
- `value_or()` 返回引用而不是副本。项目代码只传入生命周期足够长的 lvalue fallback；不要保存由临时 fallback 得到的引用。
- 只有适配要求指针的底层 API 时才使用 `get_pointer()`，不要将裸指针继续向业务层传播。

## 身份与值比较

`optional_ref` 的 `==` 和顺序比较比较目标值，不比较目标地址。判断是否引用同一对象时使用 `refers_to(object)`；比较两个 wrapper 的目标地址时使用 `gch::equal_pointer(lhs, rhs)`。
