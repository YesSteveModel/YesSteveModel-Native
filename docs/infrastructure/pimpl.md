# PImpl 与实现依赖隔离

[返回基建总览](../infrastructure.md)

主要头文件：`<pimpl.h>`。

## 适用边界与成本

`YSM_PIMPL_*` 将实现类型留在 `.cc`，用于隔离 public header 中的复杂成员和内部依赖。实现对象在外层对象内的固定容量 storage 中构造，storage 自身不进行堆分配；实现成员仍可动态分配。

容量与对齐进入外层对象布局，预留空间由每个实例承担；实现增长超过容量时必须调整声明并重新编译调用方。它不提供稳定的跨动态库 C++ ABI，边界要求见[开发指南](../development_guide.md#abi符号与生命周期)。

## 最小用法

下面的 `SampleBuffer` 将 `std::vector` 留在实现文件中，并显式提供 move-only 语义。

`sample_buffer.h`：

```cpp
#pragma once

#include <cstddef>
#include <pimpl.h>

namespace ysm {
class SampleBuffer final {
public:
    explicit SampleBuffer(std::size_t count);
    ~SampleBuffer();

    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;
    SampleBuffer(SampleBuffer&&) noexcept;
    SampleBuffer& operator=(SampleBuffer&&) noexcept;

    std::size_t Count() const;
    void Fill(float value);

private:
    YSM_PIMPL_DECLARE(Impl, 4 * sizeof(void*))
};
}  // namespace ysm
```

`sample_buffer.cc`：

```cpp
#include "sample_buffer.h"

#include <algorithm>
#include <vector>

namespace ysm {
class SampleBuffer::Impl {
public:
    explicit Impl(std::size_t count) : samples(count) {}

    std::vector<float> samples;
};

YSM_PIMPL_DEFINITION(SampleBuffer)

SampleBuffer::SampleBuffer(std::size_t count) : YSM_PIMPL_CONSTRUCT(count) {}
SampleBuffer::~SampleBuffer() = default;
SampleBuffer::SampleBuffer(SampleBuffer&&) noexcept = default;
SampleBuffer& SampleBuffer::operator=(SampleBuffer&&) noexcept = default;

std::size_t SampleBuffer::Count() const {
    return Pimpl().samples.size();
}

void SampleBuffer::Fill(float value) {
    auto& samples = Pimpl().samples;
    std::fill(samples.begin(), samples.end(), value);
}
}  // namespace ysm
```

`YSM_PIMPL_DECLARE` 放在外层类的 `private` 区域，前置声明嵌套实现类型并提供 storage 与 `Pimpl()` 声明。实现类型完整定义后，在所属 namespace 中写一次 `YSM_PIMPL_DEFINITION`，生成 const / 非 const 访问器；`YSM_PIMPL_CONSTRUCT(args...)` 用于构造函数初始化列表，将参数转发给实现类型。无参数时可省略，由 storage 默认构造实现对象。

## 容量与对齐

- `YSM_PIMPL_DECLARE(Impl, size)` 的默认对齐值是 `sizeof(std::max_align_t)`；需要显式对齐时使用 `YSM_PIMPL_DECLARE_ALIGN(Impl, size, alignment)`。
- storage 构造时静态校验容量不小于 `sizeof(Impl)`，对齐不小于且为 `alignof(Impl)` 的整数倍；显式 alignment 还必须是目标编译器支持的有效对齐值。
- 定义 `YSM_DEBUG` 时，storage 容量使用 `size * 15 / 10`，否则为 `size`。Debug 通过不能证明 Release 容量足够；所有使用该类的 translation unit 必须采用一致的配置。
- 示例的 `4 * sizeof(void*)` 是该实现的容量预算，不是 `std::vector` 的通用尺寸公式。按受支持工具链、标准库和配置验证容量与对齐；实现成员变化后重新验证，不靠无限增大预算规避布局维护。

## 生命周期与特殊成员函数

外层类的构造、析构及启用的复制 / 移动操作在头文件中声明，在 `.cc` 的实现类型完整定义后实现或 `= default`。不要直接对外层类使用在类内默认化 move 的 `YSM_MOVE_ONLY`。

storage 的复制 / 移动调用实现对象的对应构造或赋值，不是字节拷贝，也不是转移一个实现指针。外层类应显式删除不支持的操作；需要复制时，将示例中的 copy 声明改为普通声明，在 `.cc` 中默认化，并保证实现类型支持对应操作。

storage 的移动构造、移动赋值和析构固定为 `noexcept`，启用的对应实现操作必须不抛异常，否则会终止进程。移动后源实现对象仍存在，其状态遵循实现类型的 moved-from 契约；不要依赖外层对象移动后实现对象地址保持不变。
