#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>

#include <codec/audio.h>
#include <err.h>
#include <java/buffer.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>
#include <java/ref.h>
#include <scope_guard.h>

namespace ysm::lib::sound {
namespace {
constexpr const char* kResultClass =
    "com/elfmcys/ysm/natives/sound/AudioNormalizer$Output";
constexpr const char* kResultConstructor =
    "(IIILjava/lang/String;JIIIIJJIIILjava/nio/ByteBuffer;"
    "[Ljava/nio/ByteBuffer;)V";

struct ResultOwner {
    explicit ResultOwner(codec::NormalizedAudio candidate)
        : candidate(std::move(candidate)) {}

    codec::NormalizedAudio candidate;
};

struct ResultFactory {
    java::Ref<jclass> result_class;
    java::Ref<jclass> byte_buffer_class;
    jmethodID constructor{};
    jmethodID as_read_only{};
};

absl::Status PendingException(std::string_view operation) {
    return absl::InternalError("JVM rejected audio normalization " +
                               std::string(operation));
}

absl::StatusOr<ResultFactory> FindResultFactory(JNIEnv_* env) {
    auto result_class = java::LocalRef(env, env->FindClass(kResultClass));
    if (!result_class || env->ExceptionCheck()) {
        return PendingException("result class");
    }
    auto byte_buffer_class =
        java::LocalRef(env, env->FindClass("java/nio/ByteBuffer"));
    if (!byte_buffer_class || env->ExceptionCheck()) {
        return PendingException("ByteBuffer class");
    }
    const auto constructor =
        env->GetMethodID(result_class.get(), "<init>", kResultConstructor);
    const auto as_read_only = env->GetMethodID(
        byte_buffer_class.get(), "asReadOnlyBuffer", "()Ljava/nio/ByteBuffer;");
    if (constructor == nullptr || as_read_only == nullptr ||
        env->ExceptionCheck()) {
        return PendingException("result constructor");
    }
    return ResultFactory{std::move(result_class), std::move(byte_buffer_class),
                         constructor, as_read_only};
}

absl::StatusOr<java::Ref<jobject>> NewDirectReadOnly(
    JNIEnv_* env, const ResultFactory& factory, BufferViewR bytes) {
    if (bytes.empty() ||
        bytes.size() >
            static_cast<std::size_t>((std::numeric_limits<jlong>::max)())) {
        return absl::InternalError(
            "Normalized audio contains an invalid output buffer");
    }
    auto direct = java::LocalRef(
        env, env->NewDirectByteBuffer(const_cast<Byte*>(bytes.data()),
                                      static_cast<jlong>(bytes.size())));
    if (!direct || env->ExceptionCheck()) {
        return PendingException("direct buffer");
    }
    auto read_only = java::LocalRef(
        env, env->CallObjectMethod(direct.get(), factory.as_read_only));
    if (!read_only || env->ExceptionCheck()) {
        return PendingException("read-only buffer");
    }
    return read_only;
}

absl::StatusOr<java::Ref<jobjectArray>> NewChunkArray(
    JNIEnv_* env, const ResultFactory& factory, const ResultOwner* owner) {
    const auto& chunks = owner->candidate.chunks;
    if (chunks.size() >
        static_cast<std::size_t>((std::numeric_limits<jsize>::max)())) {
        return absl::ResourceExhaustedError(
            "Normalized audio has too many chunks");
    }
    auto array = java::LocalRef(
        env, env->NewObjectArray(static_cast<jsize>(chunks.size()),
                                 factory.byte_buffer_class.get(), nullptr));
    if (!array || env->ExceptionCheck()) {
        return PendingException("chunk array");
    }
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        auto buffer = NewDirectReadOnly(env, factory, chunks[index]);
        if (!buffer.ok()) {
            return buffer.status();
        }
        env->SetObjectArrayElement(array.get(), static_cast<jsize>(index),
                                   buffer->get());
        if (env->ExceptionCheck()) {
            return PendingException("chunk array element");
        }
    }
    return array;
}

std::string Diagnostic(const absl::Status& status) {
    const auto message = status.message();
    if (message.empty() || message.size() > 4'096) {
        return "Native audio normalization failed";
    }
    for (const auto value : message) {
        if (static_cast<unsigned char>(value) < 0x20U ||
            static_cast<unsigned char>(value) > 0x7eU) {
            return "Native audio normalization failed";
        }
    }
    return std::string(message);
}

absl::StatusOr<jobject> NewResult(JNIEnv_* env,
                                  codec::NormalizeAudioResult result) {
    auto factory = FindResultFactory(env);
    if (!factory.ok()) {
        return factory.status();
    }

    if (!result.ok()) {
        const auto& failure = result.failure();
        auto diagnostic_text = Diagnostic(failure.status);
        auto diagnostic =
            java::LocalRef(env, env->NewStringUTF(diagnostic_text.c_str()));
        auto chunks = java::LocalRef(
            env,
            env->NewObjectArray(0, factory->byte_buffer_class.get(), nullptr));
        if (!diagnostic || !chunks || env->ExceptionCheck()) {
            return PendingException("failure result");
        }
        auto object = java::LocalRef(
            env,
            env->NewObject(factory->result_class.get(), factory->constructor,
                           static_cast<jint>(failure.kind),
                           static_cast<jint>(failure.stage),
                           static_cast<jint>(failure.status.code()),
                           diagnostic.get(), jlong{0}, jint{0}, jint{0},
                           jint{0}, jint{0}, jlong{0}, jlong{0}, jint{0},
                           jint{0}, jint{0}, nullptr, chunks.get()));
        if (!object || env->ExceptionCheck()) {
            return PendingException("failure object");
        }
        return object.release();
    }

    auto owner = std::make_shared<ResultOwner>(std::move(result).value());
    auto first_page =
        NewDirectReadOnly(env, *factory, owner->candidate.first_page);
    if (!first_page.ok()) {
        return first_page.status();
    }
    auto chunks = NewChunkArray(env, *factory, owner.get());
    if (!chunks.ok()) {
        return chunks.status();
    }
    auto diagnostic = java::LocalRef(env, env->NewStringUTF(""));
    if (!diagnostic || env->ExceptionCheck()) {
        return PendingException("success diagnostic");
    }
    const auto handle = java::ShareOpaquePtr(owner);
    auto rollback = ScopeGuard([handle] { java::DestroyOpaquePtr(handle); });
    const auto& facts = owner->candidate.facts;
    auto object = java::LocalRef(
        env, env->NewObject(factory->result_class.get(), factory->constructor,
                            jint{-1}, jint{-1}, jint{0}, diagnostic.get(),
                            handle, static_cast<jint>(facts.codec),
                            static_cast<jint>(owner->candidate.role),
                            static_cast<jint>(facts.channels),
                            static_cast<jint>(facts.sample_rate),
                            static_cast<jlong>(facts.frames),
                            static_cast<jlong>(facts.audio_packet_bytes),
                            static_cast<jint>(facts.serial_number),
                            static_cast<jint>(facts.pre_skip),
                            static_cast<jint>(facts.output_gain),
                            first_page->get(), chunks->get()));
    if (!object || env->ExceptionCheck()) {
        return PendingException("success object");
    }
    rollback.Release();
    return object.release();
}

absl::StatusOr<jobject> Normalize(JNIEnv_* env, jobject source,
                                  jlong source_flags, jint role, jint serial,
                                  jobject cancelled) {
    if (source == nullptr || cancelled == nullptr || role < 0 || role > 1) {
        return absl::InvalidArgumentError(
            "Invalid audio normalization arguments");
    }
    const auto [size, offset, type] =
        java::BufferInputFlags::unpack(source_flags);
    static_cast<void>(size);
    static_cast<void>(offset);
    if (type != java::BufferType::kDirect) {
        return absl::InvalidArgumentError(
            "Audio normalization requires a direct source buffer");
    }
    YSM_DECLARE_OR_RETURN(
        bytes, java::BufferInput<true>::Get(env, source, source_flags));
    auto cancel_class = java::LocalRef(env, env->GetObjectClass(cancelled));
    if (!cancel_class || env->ExceptionCheck()) {
        return PendingException("cancellation token");
    }
    const auto get = env->GetMethodID(cancel_class.get(), "get", "()Z");
    if (get == nullptr || env->ExceptionCheck()) {
        return PendingException("cancellation accessor");
    }
    const auto is_cancelled = [&] {
        const auto value = env->CallBooleanMethod(cancelled, get);
        return value == JNI_TRUE || env->ExceptionCheck();
    };
    auto result = codec::NormalizeAudio(
        bytes,
        codec::NormalizeAudioOptions{
            .role = static_cast<codec::NormalizationRole>(role),
            .transcoded_serial = static_cast<std::uint32_t>(serial),
        },
        is_cancelled);
    if (env->ExceptionCheck()) {
        return PendingException("cancellation callback");
    }
    return NewResult(env, std::move(result));
}
}  // namespace

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/sound/AudioNormalizer;nNormalize(Ljava/lang/"
    "Object;JIILjava/util/concurrent/atomic/AtomicBoolean;)Lcom/elfmcys/ysm/"
    "natives/sound/AudioNormalizer$Output;",
    (source, source_flags, role, serial, cancelled)) {
    return Normalize(env, source, source_flags, role, serial, cancelled);
}
}  // namespace ysm::lib::sound
