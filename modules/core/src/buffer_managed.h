#pragma once

#include "buffer_base.h"

namespace ysm {
class BufferManaged final : public BufferBase<BufferManaged> {
    Byte* mem_ = nullptr;
    size_t cap_ = 0;
    size_t size_;

   public:
    using value_type = Byte;

    struct OwnFlag {};

    using OwnedMem = BufferView;

    static constexpr size_t kMaxSize = 256_MB;

    explicit BufferManaged() noexcept;

    explicit BufferManaged(size_t size);

    explicit BufferManaged(BufferViewR data);

    explicit BufferManaged(OwnedMem data, OwnFlag);

    // 没错，仅允许显式拷贝
    explicit BufferManaged(const BufferManaged& other)
        : BufferManaged({other.mem_, other.size_}) {}

    BufferManaged& operator=(const BufferManaged&) = delete;

    BufferManaged(BufferManaged&& other) noexcept;

    BufferManaged& operator=(BufferManaged&& other) noexcept;

    ~BufferManaged() noexcept;

    void CopyFrom(BufferViewR data);

    void resize(size_t size);

    void reserve(size_t size);

    void clear() noexcept;

    void reset() noexcept;

    void shrink_to_fit();

    [[nodiscard]] OwnedMem release() noexcept;

    [[nodiscard]] size_t capacity() const noexcept { return cap_; }

    size_t size() const noexcept { return size_; }

    Byte* data() & noexcept { return mem_; }

    const Byte* data() const& noexcept { return mem_; }

    Byte* data() && noexcept = delete;
    const Byte* data() const&& = delete;
};
}  // namespace ysm
