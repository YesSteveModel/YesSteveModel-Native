#include "result_bridge.h"

#include <absl/status/status.h>

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <java/opaque_ptr.h>
#include <java/ref.h>
#include <scope_guard.h>
#include <string_cvt.h>

#include "result_owner.h"

namespace ysm::legacy::java {
namespace {
constexpr const char* kResultClass =
    "com/elfmcys/ysm/natives/legacy/NativeLegacyImportResult$Output";
constexpr const char* kResultConstructor =
    "(ILjava/lang/String;J[B[Ljava/nio/ByteBuffer;)V";

absl::Status PendingException() {
    return absl::InternalError("JVM rejected legacy result construction");
}

absl::StatusOr<::ysm::java::Ref<jstring>> NewUtf8String(
    JNIEnv_* env, std::string_view value) {
    auto decoded = U8ToU16(value);
    if (!decoded.ok() ||
        decoded->size() >
            static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
        return absl::InvalidArgumentError("Invalid UTF-8 diagnostic");
    }
    auto string = ::ysm::java::LocalRef(
        env, env->NewString(reinterpret_cast<const jchar*>(decoded->data()),
                            static_cast<jsize>(decoded->size())));
    if (!string || env->ExceptionCheck()) {
        return PendingException();
    }
    return string;
}

absl::StatusOr<::ysm::java::Ref<jbyteArray>> NewByteArray(JNIEnv_* env,
                                                          BufferViewR bytes) {
    if (bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
        return absl::ResourceExhaustedError("Java byte array is too large");
    }
    auto array = ::ysm::java::LocalRef(
        env, env->NewByteArray(static_cast<jsize>(bytes.size())));
    if (!array || env->ExceptionCheck()) {
        return PendingException();
    }
    if (!bytes.empty()) {
        env->SetByteArrayRegion(array.get(), 0,
                                static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte*>(bytes.data()));
        if (env->ExceptionCheck()) {
            return PendingException();
        }
    }
    return array;
}

struct ResultConstructor {
    ::ysm::java::Ref<jclass> result_class;
    ::ysm::java::Ref<jclass> byte_buffer_class;
    jmethodID constructor{};
    jmethodID as_read_only{};
};

absl::StatusOr<ResultConstructor> FindResultConstructor(JNIEnv_* env) {
    auto result_class =
        ::ysm::java::LocalRef(env, env->FindClass(kResultClass));
    if (!result_class || env->ExceptionCheck()) {
        return PendingException();
    }
    auto byte_buffer_class =
        ::ysm::java::LocalRef(env, env->FindClass("java/nio/ByteBuffer"));
    if (!byte_buffer_class || env->ExceptionCheck()) {
        return PendingException();
    }
    auto constructor =
        env->GetMethodID(result_class.get(), "<init>", kResultConstructor);
    if (constructor == nullptr || env->ExceptionCheck()) {
        return PendingException();
    }
    auto as_read_only = env->GetMethodID(
        byte_buffer_class.get(), "asReadOnlyBuffer", "()Ljava/nio/ByteBuffer;");
    if (as_read_only == nullptr || env->ExceptionCheck()) {
        return PendingException();
    }
    return ResultConstructor{std::move(result_class),
                             std::move(byte_buffer_class), constructor,
                             as_read_only};
}

absl::StatusOr<::ysm::java::Ref<jobjectArray>> NewPayloadArray(
    JNIEnv_* env, const ResultConstructor& constructor,
    const ResultOwner* owner) {
    const auto& payloads = owner->payloads();
    if (payloads.size() >
        static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
        return absl::ResourceExhaustedError("Too many Java payload buffers");
    }
    auto array = ::ysm::java::LocalRef(
        env, env->NewObjectArray(static_cast<jsize>(payloads.size()),
                                 constructor.byte_buffer_class.get(), nullptr));
    if (!array || env->ExceptionCheck()) {
        return PendingException();
    }
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const auto& payload = payloads[index];
        auto direct = ::ysm::java::LocalRef(
            env, env->NewDirectByteBuffer(
                     const_cast<void*>(owner->direct_address(payload)),
                     static_cast<jlong>(payload.bytes.size())));
        if (!direct || env->ExceptionCheck()) {
            return PendingException();
        }
        auto read_only = ::ysm::java::LocalRef(
            env, env->CallObjectMethod(direct.get(), constructor.as_read_only));
        if (!read_only || env->ExceptionCheck()) {
            return PendingException();
        }
        env->SetObjectArrayElement(array.get(), static_cast<jsize>(index),
                                   read_only.get());
        if (env->ExceptionCheck()) {
            return PendingException();
        }
    }
    return array;
}

absl::StatusOr<jobject> NewResult(JNIEnv_* env,
                                  const ResultConstructor& constructor,
                                  jint status, std::string_view diagnostic,
                                  jlong handle, jbyteArray descriptor,
                                  jobjectArray payloads) {
    auto diagnostic_string = NewUtf8String(env, diagnostic);
    if (!diagnostic_string.ok()) {
        return diagnostic_string.status();
    }
    auto result = ::ysm::java::LocalRef(
        env,
        env->NewObject(constructor.result_class.get(), constructor.constructor,
                       status, diagnostic_string->get(), handle, descriptor,
                       payloads));
    if (!result || env->ExceptionCheck()) {
        return PendingException();
    }
    return result.release();
}
}  // namespace

absl::StatusOr<jobject> NewFailureResult(JNIEnv_* env,
                                         const absl::Status& status) {
    if (status.ok()) {
        return absl::InvalidArgumentError(
            "A failure result requires a non-success status");
    }
    std::string_view diagnostic = status.message();
    if (diagnostic.size() > 4096 || !U8ToU16(diagnostic).ok()) {
        diagnostic = "Native legacy import failed";
    }
    auto constructor = FindResultConstructor(env);
    if (!constructor.ok()) {
        return constructor.status();
    }
    auto descriptor = NewByteArray(env, {});
    if (!descriptor.ok()) {
        return descriptor.status();
    }
    auto payloads = ::ysm::java::LocalRef(
        env,
        env->NewObjectArray(0, constructor->byte_buffer_class.get(), nullptr));
    if (!payloads || env->ExceptionCheck()) {
        return PendingException();
    }
    jint code = 8;
    switch (status.code()) {
        case absl::StatusCode::kUnimplemented:
            code = 2;
            break;
        case absl::StatusCode::kInvalidArgument:
        case absl::StatusCode::kDataLoss:
            code = 3;
            break;
        case absl::StatusCode::kResourceExhausted:
            code = 4;
            break;
        case absl::StatusCode::kNotFound:
        case absl::StatusCode::kPermissionDenied:
        case absl::StatusCode::kUnavailable:
            code = 5;
            break;
        case absl::StatusCode::kFailedPrecondition:
            code = 11;
            break;
        default:
            break;
    }
    return NewResult(env, *constructor, code, diagnostic, 0, descriptor->get(),
                     payloads.get());
}

absl::StatusOr<jobject> NewSuccessResult(JNIEnv_* env,
                                         v3::ImportResult import_result) {
    auto created = ResultOwner::Create(std::move(import_result));
    if (!created.ok()) {
        return created.status();
    }
    std::shared_ptr<ResultOwner> owner(std::move(*created));
    auto constructor = FindResultConstructor(env);
    if (!constructor.ok()) {
        return constructor.status();
    }
    auto descriptor = NewByteArray(env, owner->descriptor());
    if (!descriptor.ok()) {
        return descriptor.status();
    }
    auto payloads = NewPayloadArray(env, *constructor, owner.get());
    if (!payloads.ok()) {
        return payloads.status();
    }

    const auto handle = ::ysm::java::ShareOpaquePtr(owner);
    auto rollback =
        ScopeGuard([handle] { ::ysm::java::DestroyOpaquePtr(handle); });
    auto transport_result = NewResult(env, *constructor, 0, "", handle,
                                      descriptor->get(), payloads->get());
    if (!transport_result.ok()) {
        return transport_result.status();
    }
    rollback.Release();
    return transport_result;
}
}  // namespace ysm::legacy::java
