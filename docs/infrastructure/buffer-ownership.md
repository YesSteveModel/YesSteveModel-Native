# Buffer、所有权与字节视图

[返回基建总览](../infrastructure.md)

主要头文件：`<buffer.h>`、`<buffer_managed.h>`、`<buffer_scalar.h>`、`<c_string_view.h>`、`<string_cvt.h>`、`<fs.h>`、`<system_allocator.h>`。

## Buffer

| 类型/函数 | 契约 |
| --- | --- |
| `BufferView` / `BufferViewR` | 可写/只读 borrowed span，不延长底层生命周期 |
| `BufferFixed<N>` 及 fixed view | 长度由类型固定 |
| `BufferManaged` | 普通分配路径限制为 256 MiB；move 优先，复制需显式构造，禁止复制赋值 |
| `BufferBase<Derived>` | 提供 span 转换和迭代；禁止从临时对象取得 view、迭代器或引用 |
| `BufferScalar<T>` / `ScalarBuf` / `BufScalar` | trivially-copyable scalar 与主机对象表示的字节视图互转，不是通用 wire 编码 |
| `Slice(buffer, offset, size)` / `Consume(view, size)` | 建立或推进 borrowed 子视图；`size` 可为尺寸字面量或运行期 `size_t`，调用前保证范围有效 |
| `BufStr` / `StrBuf` | 字节与字符串零拷贝互转；`StrBuf` 拒绝临时 `std::string` |
| `Cmp` / `Copy` | 比较和复制；`Copy` 要求目标容量足够 |
| `_B` / `_KB` / `_MB` / `_GB` | 编译期尺寸字面量 |

```cpp
BufferManaged packet(64_KB);
auto cursor = BufferView(packet);
auto header = Consume(cursor, 8_B);
size_t payload_size = cursor.size();
auto payload = Slice(cursor, 0, payload_size);
```

`Slice` 和 `Consume` 的尺寸字面量重载返回 `BufferFixedView<N>`，只读输入返回 `BufferFixedViewR<N>`；运行期 `size_t` 重载分别返回 `BufferView` 或 `BufferViewR`。上例中 `header` 为 `BufferFixedView<8>`，`payload` 为 `BufferView`。`Slice(buffer, offset)` 也走动态重载并返回从 `offset` 到末尾的 `BufferView` / `BufferViewR`。fixed 与 dynamic 返回类型只表达编译期是否已知长度，两类 view 都不延长 backing 生命周期。

`BufferManaged` 的接口约定与 `std::vector<Byte>` 基本相同：`size()` / `capacity()` 分离有效长度与容量，`reserve()` 不改变有效长度，`resize()` 改变有效长度并按需扩容，`clear()` 保留容量，`shrink_to_fit()` 收缩到有效长度。可能重分配的操作会使已有 view 失效。项目特有差异是 `BufferManaged(size)` 和 `resize()` 不初始化新增字节，写出或交给消费者前必须填满有效范围；普通分配路径限制为 `kMaxSize`，超限或分配失败会抛 `std::bad_alloc`，不会返回 status。异常在[开发指南](../development_guide.md#错误模型与热点路径)规定的边界收敛。

`clear()` 保留容量，`reset()` 释放内存；`release()` 返回携带当前有效长度的 `OwnedMem` 并清空原 owner，接收方负责最终释放。`OwnedMem` 是 view，不会自行释放。

`BufferManaged(OwnedMem, OwnFlag{})` 接管已有 allocation，不复制、不检查分配器或 256 MiB 上限。调用方必须先验证长度，并传入可由项目分配器释放的 allocation 起始地址；不能接管 slice、栈内存或仍由其他 owner 管理的地址。

`SystemAllocator<T>` 绕过 mimalloc；不得混用不兼容的分配与释放路径。其强制使用场景见[开发指南](../development_guide.md#abi符号与生命周期)。

## 字符串与路径

`CStringView` 是保证 NUL 结尾的 borrowed view；底层字符必须保持存活且地址稳定。

`U8ToU16` 和 `U16ToU8` 严格校验输入，保留嵌入的 NUL。合法空输入返回空字符串。非法输入返回 `InvalidArgument`，错误消息中的偏移量从 0 开始，单位分别为 UTF-8 字节和 UTF-16 码元。

`U8ToNative`、`U16ToNative`、`NativeToU8`、`PathToU8` 和 `U8ToPath` 处理本地编码和路径。

## 文件 I/O

| 入口 | 契约 |
| --- | --- |
| `FileRead(path, dst)` | 仅读取普通文件；先清空 `dst`；拒绝超过 256 MiB 的文件 |
| `FileSize(path)` | 仅接受普通文件；平台错误映射为 status |
| `FileWrite(path, data)` | 二进制截断写入；不保证原子替换，不创建父目录 |
