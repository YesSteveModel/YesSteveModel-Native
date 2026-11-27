#pragma once

#include <absl/status/statusor.h>
#include <jni.h>
#include <any>
#include <memory>

#include "err.h"

namespace ysm::java {
static_assert(sizeof(jlong) >= sizeof(std::nullptr_t));

template <typename T, typename... Args>
jlong MakeOpaquePtr(Args&&... args) {
    return reinterpret_cast<jlong>(
        new std::any(std::make_shared<T>(std::forward<Args>(args)...)));
}

inline jlong ShareOpaquePtr(jlong ptr) {
    if (!ptr) [[unlikely]] {
        return 0;
    }
    return reinterpret_cast<jlong>(
        new std::any(*reinterpret_cast<const std::any*>(ptr)));
}

template <typename T>
jlong ShareOpaquePtr(const std::shared_ptr<T>& ptr) {
    if (!ptr) [[unlikely]] {
        return 0;
    }
    return reinterpret_cast<jlong>(new std::any(ptr));
}

inline void DestroyOpaquePtr(jlong addr) {
    if (addr) [[likely]] {
        delete reinterpret_cast<std::any*>(addr);
    }
}

template <typename T>
std::shared_ptr<T>* TryCastOpaquePtr(jlong addr) noexcept {
    return std::any_cast<std::shared_ptr<T>>(reinterpret_cast<std::any*>(addr));
}

template <typename T>
absl::StatusOr<std::reference_wrapper<std::shared_ptr<T>>> CastOpaquePtr(
    jlong addr) {
    auto* any = reinterpret_cast<std::any*>(addr);
    YSM_ASSERT(
        any, absl::InvalidArgumentError("Cannot cast a null opaque pointer."));

    auto* ptr = TryCastOpaquePtr<T>(addr);
    YSM_ASSERT(ptr, absl::InvalidArgumentError(std::format(
                        "Cannot cast opaque type {} to {}", any->type().name(),
                        typeid(std::shared_ptr<T>).name())));

    auto& handle = *ptr;
    YSM_ASSERT(handle, absl::FailedPreconditionError(
                           "Opaque pointer contains a null shared_ptr."));

    return std::ref(handle);
}

template <typename T>
absl::StatusOr<std::shared_ptr<T>> AcquireOpaquePtr(jlong addr) {
    YSM_DECLARE_OR_RETURN(res, CastOpaquePtr<T>(addr));
    return res;
}
}  // namespace ysm::java
