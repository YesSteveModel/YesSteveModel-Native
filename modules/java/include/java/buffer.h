#pragma once

#include <algorithm>
#include <jni.h>

#include "array.h"
#include "bitpack.h"
#include "buffer_base.h"
#include "buffer_managed.h"
#include "empty.h"
#include "move_only.h"
#include "non_copyable.h"

namespace ysm::java {
enum class BufferType : uint32_t { kByteArray = 0, kDirect = 1 };

YSM_BIT_PACK(BufferInputFlags, jlong,
             YSM_BIT_FIELD(size, uint32_t, 32),
             YSM_BIT_FIELD(offset, uint32_t, 31),
             YSM_BIT_FIELD(type, BufferType, 1));

template <bool kReadOnly>
class CriticalBuffer : public BufferBase<CriticalBuffer<kReadOnly>> {
    CriticalByteArray<kReadOnly> array_;

    CriticalBuffer(CriticalByteArray<kReadOnly> array)
        : array_(std::move(array)) {}

   public:
    CriticalBuffer() = default;
    YSM_MOVE_ONLY(CriticalBuffer);

    bool empty() const noexcept { return array_.empty(); }

    void clear() {
        array_.clear();
    }

    jbyteArray release() noexcept {
        return array_.release();
    }

    Byte* data() { return reinterpret_cast<Byte*>(array_.data()); }

    const Byte* data() const { return reinterpret_cast<const Byte*>(array_.data()); }

    size_t size() const { return array_.size(); }

    jbyteArray obj() const { return array_.obj(); }

    static absl::StatusOr<CriticalBuffer> Get(
            JNIEnv_* env, jbyteArray buf, jint offset = 0,
            std::optional<jint> size = std::nullopt) noexcept {
        YSM_DECLARE_OR_RETURN(critical, CriticalByteArray<kReadOnly>::Get(env, buf, offset, size));
        return CriticalBuffer{std::move(critical)};
    }
};

inline absl::StatusOr<BufferView> GetDirectBuffer(
    JNIEnv_* env, jobject direct_buffer, jint offset = 0,
    std::optional<jint> size = std::nullopt) noexcept {
    auto cap = env->GetDirectBufferCapacity(direct_buffer);
    YSM_ASSERT(cap >= 0, kEmptyBuffer);
    YSM_ASSERT(offset >= 0,
               absl::InvalidArgumentError("Invalid buffer offset"sv));
    auto remaining = cap - offset;
    auto slice_size = size.value_or(remaining);
    YSM_ASSERT(slice_size >= 0 && remaining >= slice_size,
               absl::InvalidArgumentError("Invalid buffer size"sv));

    auto ptr = env->GetDirectBufferAddress(direct_buffer);
    YSM_ASSERT(ptr != nullptr || slice_size == 0,
               absl::InternalError("Failed to get direct buffer ptr"sv));

    return BufferView{slice_size == 0 ? nullptr
                                     : static_cast<Byte*>(ptr) + offset,
                      static_cast<size_t>(slice_size)};
}

template <bool kReadOnly, bool kSafe = false>
class BufferInput : public BufferBase<BufferInput<kReadOnly, kSafe>> {
    YSM_EMPTY_OR(!kSafe, CriticalBuffer<kReadOnly>) byte_array_;
    YSM_EMPTY_OR(kSafe, BufferManaged) cache_;
    BufferView mem_;

    template <typename = void> requires(!kSafe)
    BufferInput(CriticalBuffer<kReadOnly> byte_array)
        : byte_array_(std::move(byte_array)), mem_(byte_array_) {}

    template <typename = void> requires(kSafe)
    BufferInput(BufferManaged cache)
        : cache_(std::move(cache)), mem_(cache_) {}

    BufferInput(BufferView mem) : mem_(mem) {}

   public:
    YSM_MOVE_ONLY(BufferInput);

    template <typename = void>
        requires(!kReadOnly)
    Byte* data() {
        return mem_.data();
    }

    Byte* data() const { return mem_.data(); }

    size_t size() const { return mem_.size(); }

    static absl::StatusOr<BufferInput> Get(JNIEnv_* env,
            jobject buf, jlong buf_flags, std::optional<size_t> required = std::nullopt) noexcept {
        const auto [size, offset, type] =
            BufferInputFlags::unpack(buf_flags);
        YSM_BIT_ENUM_VALIDATE(type);
        YSM_ASSERT(!required || size >= required.value(),
                   absl::ResourceExhaustedError("Buffer size too small"));
        YSM_ASSERT(env != nullptr && buf != nullptr,
                   absl::InvalidArgumentError("Null"sv));
        if (type == BufferType::kByteArray) {
            auto array = reinterpret_cast<jbyteArray>(buf);
            if constexpr (kSafe) {
                BufferManaged cache(size);
                YSM_RETURN_IF_ERROR(ReadByteArray<true>(
                    env, array, cache, static_cast<jint>(offset)));
                return BufferInput(std::move(cache));
            } else {
                YSM_DECLARE_OR_RETURN(
                    critical_buf,
                    CriticalBuffer<kReadOnly>::Get(env, array, offset, size));
                return BufferInput(std::move(critical_buf));
            }
        } else {
            YSM_DECLARE_OR_RETURN(mem, GetDirectBuffer(env, buf, offset, size));
            return BufferInput(mem);
        }
    }
};

template <bool kSafe = false>
class BufferOutput : public BufferBase<BufferOutput<kSafe>>, NonCopyable {
    YSM_EMPTY_OR(!kSafe, CriticalBuffer<false>) byte_array_;
    BufferManaged native_buffer_;
    JNIEnv_* env_;
    BufferView mem_;
    BufferType type_;

    template <typename = void>
        requires(!kSafe)
    BufferOutput(JNIEnv_* env, CriticalBuffer<false> byte_array)
        : byte_array_(std::move(byte_array)),
          env_(env),
          mem_(byte_array_),
          type_(BufferType::kByteArray) {}

    BufferOutput(JNIEnv_* env, BufferManaged native_buf, BufferType type)
        : native_buffer_(std::move(native_buf)),
          env_(env),
          mem_(native_buffer_),
          type_(type) {}

   public:
    BufferOutput(const BufferOutput&) = delete;
    BufferOutput& operator=(const BufferOutput&) = delete;

    BufferOutput(BufferOutput&& other) noexcept
        : env_(other.env_),
          mem_(other.mem_),
          native_buffer_(std::move(other.native_buffer_)),
          byte_array_(std::move(other.byte_array_)),
          type_(other.type_) {
        other.clear();
    }

    BufferOutput& operator=(BufferOutput&& other) noexcept {
        if (this != &other) {
            native_buffer_ = std::move(other.native_buffer_);
            byte_array_ = std::move(other.byte_array_);
            env_ = other.env_;
            type_ = other.type_;
            other.clear();
        }
        return *this;
    }

    void clear() noexcept {
        native_buffer_.reset();
        env_ = nullptr;
        if constexpr (!kSafe) {
            byte_array_.clear();
        }
    }

    Byte* data() { return mem_.data(); }

    Byte* data() const { return mem_.data(); }

    size_t size() const { return mem_.size(); }

    jobject release() {
        if (type_ == BufferType::kDirect) {
            auto output = env_->NewDirectByteBuffer(
                native_buffer_.data(), native_buffer_.size());
            if (output != nullptr) {
                mem_ = {};
                [[maybe_unused]] auto transferred = native_buffer_.release();
            }
            return output;
        } else {
            mem_ = {};
            if constexpr (kSafe) {
                auto array = env_->NewByteArray(native_buffer_.size());
                if (array == nullptr) {
                    return nullptr;
                }
                env_->SetByteArrayRegion(
                    array, 0, native_buffer_.size(),
                    reinterpret_cast<jbyte*>(native_buffer_.data()));
                return array;
            } else {
                return byte_array_.release();
            }
        }
    }

    static absl::StatusOr<BufferOutput> Create(JNIEnv_* env, jint size,
                                                BufferType type) noexcept {
        YSM_ASSERT(env != nullptr && size >= 0,
                    absl::InvalidArgumentError("Null"sv));
        if (type == BufferType::kByteArray) {
            if constexpr (kSafe) {
                return BufferOutput(env, BufferManaged(size),
                                    BufferType::kByteArray);
            } else {
                auto array = env->NewByteArray(size);
                YSM_DECLARE_OR_RETURN(critical_buf,
                                      CriticalBuffer<false>::Get(env, array));
                return BufferOutput(env, std::move(critical_buf));
            }
        } else {
            BufferManaged buffer(std::max(size, 1));
            buffer.resize(size);
            return BufferOutput(env, std::move(buffer),
                                BufferType::kDirect);
        }
    }
};

static absl::StatusOr<jobject> TryMoveToOutput(JNIEnv_* env, BufferManaged buf, BufferType type) noexcept {
    YSM_ASSERT(env != nullptr, absl::InvalidArgumentError("Null"sv));
    if (env->ExceptionCheck()) {
        return nullptr;
    }
    if (type == BufferType::kByteArray) {
        auto array = env->NewByteArray(buf.size());
        if (array == nullptr) {
            return nullptr;
        }
        env->SetByteArrayRegion(array, 0, buf.size(),
            reinterpret_cast<jbyte*>(buf.data()));
        return array;
    } else {
        auto output = env->NewDirectByteBuffer(buf.data(), buf.size());
        if (output == nullptr || env->ExceptionCheck()) {
            if (output != nullptr) {
                env->DeleteLocalRef(output);
            }
            return nullptr;
        }
        [[maybe_unused]] auto transferred = buf.release();
        return output;
    }
}

static absl::StatusOr<jobject> CopyToOutput(JNIEnv_* env, BufferViewR buf, BufferType type) noexcept {
    YSM_ASSERT(env != nullptr, absl::InvalidArgumentError("Null"sv));
    if (type == BufferType::kByteArray) {
        auto array = env->NewByteArray(buf.size());
        if (array == nullptr) {
            return nullptr;
        }
        env->SetByteArrayRegion(array, 0, buf.size(),
                                reinterpret_cast<const jbyte*>(buf.data()));
        return array;
    } else {
        BufferManaged copy(buf);
        auto output = env->NewDirectByteBuffer(copy.data(), copy.size());
        if (output != nullptr) {
            [[maybe_unused]] auto transferred = copy.release();
        }
        return output;
    }
}
}  // namespace ysm::java
