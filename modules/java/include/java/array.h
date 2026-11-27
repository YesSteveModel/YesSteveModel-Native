#pragma once

#include <jni.h>

#include <algorithm>
#include <concepts>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "err.h"
#include "ref.h"
#include "scope_guard.h"

namespace ysm::java {
namespace internal {
template <typename From, typename To>
concept StaticCastableTo = requires { static_cast<To>(std::declval<From>()); };

template <typename From, typename To>
inline constexpr bool kPrimitiveArrayCopyCompatible =
    sizeof(From) == sizeof(To) &&
    (std::is_same_v<From, To> ||
     (std::is_integral_v<From> && std::is_integral_v<To>));

inline absl::StatusOr<jsize> ValidatePrimitiveArrayRange(
    JNIEnv_* env, jarray array, jint offset, size_t size) noexcept {
    YSM_ASSERT(env != nullptr && array != nullptr,
               absl::InvalidArgumentError("Null"sv));
    YSM_ASSERT(offset >= 0,
               absl::InvalidArgumentError("Invalid Java array offset."sv));
    auto array_size = env->GetArrayLength(array);
    YSM_ASSERT(static_cast<size_t>(offset) <=
                   static_cast<size_t>(array_size) &&
                   size <= static_cast<size_t>(array_size - offset),
               absl::InvalidArgumentError("Invalid Java array range."sv));
    return static_cast<jsize>(size);
}
}  // namespace internal

template <std::ranges::sized_range Vec>
Ref<jobjectArray> ObjectArray(JNIEnv_* env, jclass type, const Vec& list, auto&& func) {
    auto size = static_cast<jsize>(std::ranges::size(list));
    auto array = env->NewObjectArray(size, type, nullptr);
    auto i = 0;
    for (auto& element : list) {
        Ref<jobject> obj = func(env, element);
        env->SetObjectArrayElement(array, i++, obj.get());
    }
    return LocalRef(env, array);
}

Ref<jobjectArray> ObjectArray(JNIEnv_* env, jclass type, jsize size, auto&& func) {
    auto array = env->NewObjectArray(size, type, nullptr);
    for (auto i = 0; i < size; i++) {
        Ref<jobject> obj = func(env, i);
        env->SetObjectArrayElement(array, i, obj.get());
    }
    return LocalRef(env, array);
}

#define PRIMITIVE_ARRAY(TYPE, NAME)                                              \
    template <bool kSafe = false, std::ranges::sized_range Vec,                 \
              typename Func>                                                    \
        requires std::invocable<Func&, std::ranges::range_reference_t<Vec>>    \
    absl::Status Write##NAME##Array(JNIEnv_* env, TYPE##Array array,            \
                                    const Vec& list, Func&& func,               \
                                    jint offset = 0) {                          \
        YSM_DECLARE_OR_RETURN(                                                  \
            size, internal::ValidatePrimitiveArrayRange(                       \
                      env, array, offset,                                       \
                      static_cast<size_t>(std::ranges::size(list))));           \
        if (size == 0) {                                                        \
            return OkStatus();                                                  \
        }                                                                       \
        if constexpr (kSafe) {                                                  \
            std::vector<TYPE> vec(static_cast<size_t>(size));                   \
            std::ranges::copy(list | std::views::transform(func), vec.begin()); \
            env->Set##NAME##ArrayRegion(array, offset, size, vec.data());       \
        } else {                                                                \
            jboolean is_copy = false;                                           \
            auto region = env->GetPrimitiveArrayCritical(array, &is_copy);      \
            YSM_ASSERT(region != nullptr,                                       \
                       absl::ResourceExhaustedError(                            \
                           "Failed to get Java array pointer."sv));            \
            auto guard = ScopeGuard([&] {                                      \
                env->ReleasePrimitiveArrayCritical(array, region, 0);           \
            });                                                                 \
            std::span span(static_cast<TYPE*>(region) + offset,                 \
                           static_cast<size_t>(size));                           \
            std::ranges::copy(list | std::views::transform(func), span.begin()); \
        }                                                                       \
        return OkStatus();                                                      \
    }                                                                           \
                                                                                \
    template <bool kSafe = false, std::ranges::sized_range Vec,                 \
              typename ValueType = std::ranges::range_value_t<Vec>>             \
        requires internal::StaticCastableTo<ValueType, TYPE>                    \
    absl::Status Write##NAME##Array(JNIEnv_* env, TYPE##Array array,            \
                                    const Vec& list, jint offset = 0) {         \
        if constexpr (std::ranges::contiguous_range<Vec> &&                     \
                      internal::kPrimitiveArrayCopyCompatible<ValueType,        \
                                                              TYPE>) {          \
            YSM_DECLARE_OR_RETURN(                                              \
                size, internal::ValidatePrimitiveArrayRange(                   \
                          env, array, offset,                                   \
                          static_cast<size_t>(std::ranges::size(list))));       \
            if (size != 0) {                                                    \
                std::span span = list;                                          \
                env->Set##NAME##ArrayRegion(                                    \
                    array, offset, size,                                        \
                    reinterpret_cast<const TYPE*>(span.data()));                \
            }                                                                   \
            return OkStatus();                                                  \
        } else {                                                                \
            return Write##NAME##Array<kSafe>(                                  \
                env, array, list, [](const ValueType& value) {                  \
                    return static_cast<TYPE>(value);                            \
                }, offset);                                                     \
        }                                                                       \
    }                                                                           \
                                                                                \
    template <bool kSafe = false, std::ranges::sized_range Vec,                 \
              typename Func>                                                    \
        requires std::invocable<Func&, TYPE>                                   \
    absl::Status Read##NAME##Array(JNIEnv_* env, TYPE##Array array, Vec& list,  \
                                   Func&& func, jint offset = 0) {              \
        YSM_DECLARE_OR_RETURN(                                                  \
            size, internal::ValidatePrimitiveArrayRange(                       \
                      env, array, offset,                                       \
                      static_cast<size_t>(std::ranges::size(list))));           \
        if (size == 0) {                                                        \
            return OkStatus();                                                  \
        }                                                                       \
        if constexpr (kSafe) {                                                  \
            std::vector<TYPE> vec(static_cast<size_t>(size));                   \
            env->Get##NAME##ArrayRegion(array, offset, size, vec.data());       \
            std::ranges::copy(vec | std::views::transform(func),               \
                              std::ranges::begin(list));                        \
        } else {                                                                \
            jboolean is_copy = false;                                           \
            auto region = env->GetPrimitiveArrayCritical(array, &is_copy);      \
            YSM_ASSERT(region != nullptr,                                       \
                       absl::ResourceExhaustedError(                            \
                           "Failed to get Java array pointer."sv));            \
            auto guard = ScopeGuard([&] {                                      \
                env->ReleasePrimitiveArrayCritical(array, region, JNI_ABORT);   \
            });                                                                 \
            std::span span(static_cast<const TYPE*>(region) + offset,           \
                           static_cast<size_t>(size));                           \
            std::ranges::copy(span | std::views::transform(func),              \
                              std::ranges::begin(list));                        \
        }                                                                       \
        return OkStatus();                                                      \
    }                                                                           \
                                                                                \
    template <bool kSafe = false, std::ranges::sized_range Vec,                 \
              typename ValueType = std::ranges::range_value_t<Vec>>             \
        requires internal::StaticCastableTo<TYPE, ValueType>                    \
    absl::Status Read##NAME##Array(JNIEnv_* env, TYPE##Array array, Vec& list,  \
                                   jint offset = 0) {                           \
        if constexpr (std::ranges::contiguous_range<Vec> &&                     \
                      internal::kPrimitiveArrayCopyCompatible<TYPE,             \
                                                              ValueType>) {     \
            YSM_DECLARE_OR_RETURN(                                              \
                size, internal::ValidatePrimitiveArrayRange(                   \
                          env, array, offset,                                   \
                          static_cast<size_t>(std::ranges::size(list))));       \
            if (size != 0) {                                                    \
                env->Get##NAME##ArrayRegion(                                    \
                    array, offset, size,                                        \
                    reinterpret_cast<TYPE*>(std::ranges::data(list)));          \
            }                                                                   \
            return OkStatus();                                                  \
        } else {                                                                \
            return Read##NAME##Array<kSafe>(                                   \
                env, array, list, [](TYPE value) {                              \
                    return static_cast<ValueType>(value);                       \
                }, offset);                                                     \
        }                                                                       \
    }                                                                           \
                                                                                \
    template <bool kSafe = false, std::ranges::sized_range Vec>                 \
    Ref<TYPE##Array> NAME##Array(JNIEnv_* env, const Vec& list, auto&& func) {  \
        auto size = static_cast<jsize>(std::ranges::size(list));                \
        auto array = LocalRef(env, env->New##NAME##Array(size));                \
        if (!array) [[unlikely]] {                                              \
            throw std::bad_alloc();                                             \
        }                                                                       \
        auto status = Write##NAME##Array<kSafe>(env, array.get(), list,         \
                                                 std::forward<decltype(func)>(func)); \
        if (!status.ok()) [[unlikely]] {                                        \
            throw std::invalid_argument(status.ToString());                     \
        }                                                                       \
        return array;                                                           \
    }                                                                           \
                                                                                \
    template <bool kSafe = false, std::ranges::sized_range Vec,                 \
              typename ValueType = std::ranges::range_value_t<Vec>>             \
        requires internal::StaticCastableTo<ValueType, TYPE>                    \
    Ref<TYPE##Array> NAME##Array(JNIEnv_* env, const Vec& list) {               \
        auto size = static_cast<jsize>(std::ranges::size(list));                \
        auto array = LocalRef(env, env->New##NAME##Array(size));                \
        if (!array) [[unlikely]] {                                              \
            throw std::bad_alloc();                                             \
        }                                                                       \
        auto status = Write##NAME##Array<kSafe>(env, array.get(), list);        \
        if (!status.ok()) [[unlikely]] {                                        \
            throw std::invalid_argument(status.ToString());                     \
        }                                                                       \
        return array;                                                           \
    } \
                                                                                \
    template <bool kReadOnly>    \
     class Critical##NAME##Array {    \
         JNIEnv_* env_;    \
         TYPE##Array obj_;    \
         size_t offset_;    \
         size_t length_;    \
         TYPE* mem_;    \
         \
         Critical##NAME##Array(JNIEnv_* env, TYPE##Array array, size_t offset, size_t length,    \
                        TYPE* mem)    \
             : env_(env), obj_(array), offset_(offset), length_(length), mem_(mem) {}    \
         \
        public:    \
         Critical##NAME##Array() : Critical##NAME##Array(nullptr, nullptr, 0, 0, nullptr) {}    \
         \
         Critical##NAME##Array(const Critical##NAME##Array&) = delete;    \
         Critical##NAME##Array& operator=(const Critical##NAME##Array&) = delete;    \
         \
         Critical##NAME##Array(Critical##NAME##Array&& other) noexcept    \
             : env_(other.env_),    \
               obj_(other.obj_),    \
               offset_(other.offset_),    \
               length_(other.length_),    \
               mem_(other.mem_) {    \
             other.clear(false);    \
         }    \
         \
         Critical##NAME##Array& operator=(Critical##NAME##Array&& other) noexcept {    \
             if (this != &other) {    \
                 clear();    \
                 mem_ = other.mem_;    \
                 length_ = other.length_;    \
                 obj_ = other.obj_;    \
                 offset_ = other.offset_;    \
                 env_ = other.env_;    \
                 other.clear(false);    \
             }    \
             return *this;    \
         }    \
             \
         TYPE* data() {    \
             return mem_ == nullptr ? nullptr : mem_ + offset_;    \
         }    \
         const TYPE* data() const {    \
             return mem_ == nullptr ? nullptr : mem_ + offset_;    \
         }    \
             \
         size_t size() const {    \
             return length_;    \
         }    \
         \
         std::span<TYPE> span() {    \
             return {data(), size()};    \
         }    \
             \
         std::span<const TYPE> span() const {    \
             return {data(), size()};    \
         }    \
         \
         bool empty() const noexcept { return mem_ == nullptr; }    \
         \
         TYPE##Array release() noexcept {    \
             auto obj = obj_;    \
             clear();    \
             return obj;    \
         }    \
         \
         ~Critical##NAME##Array() noexcept { clear(true); }    \
         \
         TYPE##Array obj() const { return obj_; }    \
         \
         void clear() {    \
             clear(true);    \
         }    \
         \
         static absl::StatusOr<Critical##NAME##Array> Get(    \
             JNIEnv_* env, TYPE##Array buf, jint offset = 0,    \
             std::optional<jint> length = std::nullopt) noexcept {    \
             YSM_ASSERT(env != nullptr && buf != nullptr,    \
                        absl::InvalidArgumentError("Null"sv));    \
             auto cap = static_cast<size_t>(env->GetArrayLength(buf));    \
             YSM_ASSERT(offset >= 0 && static_cast<size_t>(offset) <= cap,    \
                        absl::InvalidArgumentError("Invalid array offset"sv));    \
             auto remaining = cap - static_cast<size_t>(offset);    \
             auto slice_length =    \
                 length.value_or(static_cast<jint>(remaining));    \
             YSM_ASSERT(slice_length >= 0 &&    \
                            static_cast<size_t>(slice_length) <= remaining,    \
                        absl::InvalidArgumentError("Invalid array length"sv));    \
             return PinValidatedRange(env, buf, offset, slice_length);    \
         }    \
         \
         /* The caller has already checked this range against this array's length. */    \
         static absl::StatusOr<Critical##NAME##Array> PinValidatedRange(    \
             JNIEnv_* env, TYPE##Array buf, jint offset, jint slice_length) noexcept {    \
             void* mem;    \
             if (slice_length > 0) [[likely]] {    \
                 jboolean is_copy = false;    \
                 mem = env->GetPrimitiveArrayCritical(buf, &is_copy);    \
                 YSM_ASSERT(mem != nullptr,    \
                            absl::InvalidArgumentError("Failed to get array ptr"sv));    \
             } else {    \
                 mem = nullptr;    \
             }    \
             return Critical##NAME##Array{env, buf, static_cast<size_t>(offset),    \
                                   static_cast<size_t>(slice_length),    \
                                   static_cast<TYPE*>(mem)};    \
         }    \
         \
     private:    \
         void clear(bool leave) noexcept {    \
             if (leave && mem_ != nullptr) {    \
                 env_->ReleasePrimitiveArrayCritical(obj_, mem_,    \
                                                     kReadOnly ? JNI_ABORT : 0);    \
             }    \
             mem_ = nullptr;    \
             length_ = 0;    \
             obj_ = nullptr;    \
             offset_ = 0;    \
             env_ = nullptr;    \
         }    \
     };

PRIMITIVE_ARRAY(jboolean, Boolean)
PRIMITIVE_ARRAY(jbyte, Byte)
PRIMITIVE_ARRAY(jchar, Char)
PRIMITIVE_ARRAY(jint, Int)
PRIMITIVE_ARRAY(jshort, Short)
PRIMITIVE_ARRAY(jlong, Long)
PRIMITIVE_ARRAY(jfloat, Float)
PRIMITIVE_ARRAY(jdouble, Double)

#undef PRIMITIVE_ARRAY
}  // namespace ysm::java
