#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include <codec/opus.h>
#include <err.h>
#include <java/buffer.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>

namespace ysm::lib::sound {
namespace {
std::atomic<jlong> create_count{};
std::atomic<jlong> restore_count{};
std::atomic<bool> fail_next_restore{};
std::atomic<bool> pause_next_restore{};
std::atomic<bool> restore_paused{};

struct RestoreGate {
    std::mutex mutex;
    std::condition_variable resumed;
    bool released{true};
};

RestoreGate& GetRestoreGate() {
    static RestoreGate gate;
    return gate;
}

void PauseRestore() {
    auto& gate = GetRestoreGate();
    std::unique_lock lock(gate.mutex);
    restore_paused.store(true, std::memory_order_release);
    gate.resumed.wait(lock, [&] { return gate.released; });
    restore_paused.store(false, std::memory_order_release);
}

absl::StatusOr<jboolean> RestorePaused(JNIEnv_*) {
    return restore_paused.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

// A false poll is normal state, whereas the macro's Z form maps failure status
// to false. Use the same registrar with a boolean value to avoid failure logs.
[[maybe_unused]] java::EntryRegistrar<
    &RestorePaused,
    "Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nRestorePaused()Z">
    restore_paused_entry;

// Include the opaque any, its shared_ptr payload, and a conservative owner
// header for the supported standard libraries.
// Allocator bookkeeping is excluded, as for the PCM backing byte budget.
constexpr std::size_t kCheckpointHandleBytes =
    sizeof(std::any) + sizeof(std::shared_ptr<codec::OpusAudioCheckpoint>) +
    4 * sizeof(void*);

void ThrowOpusException(JNIEnv_* env, CStringView type, CStringView message) {
    auto exception_class = java::LocalRef(env, env->FindClass(type.c_str()));
    if (exception_class && !env->ExceptionCheck()) {
        env->ThrowNew(exception_class.get(), message.c_str());
    }
}

void ThrowCheckpointContract(JNIEnv_* env, const absl::Status& status) {
    const auto message = status.ToString();
    ThrowOpusException(env, "java/lang/IllegalStateException"_sv, message);
}
}  // namespace

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nCreate(J)J",
              (expected_frames)) {
    create_count.fetch_add(1, std::memory_order_relaxed);
    YSM_ASSERT(expected_frames >= 0,
               absl::InvalidArgumentError("Negative Opus frame count"sv));
    return java::MakeOpaquePtr<codec::OpusAudioStream>(
        static_cast<uint64_t>(expected_frames));
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nCheckpointSize(J)J",
              (ptr), jlong{-1}) {
    YSM_DECLARE_OR_RETURN(decoder,
                          java::CastOpaquePtr<codec::OpusAudioStream>(ptr));
    const auto size = decoder->CheckpointSize();
    if (!size.ok()) {
        ThrowCheckpointContract(env, size.status());
        return size.status();
    }
    return static_cast<jlong>(*size + kCheckpointHandleBytes);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nCaptureCheckpoint(J)J",
              (ptr)) {
    try {
        YSM_DECLARE_OR_RETURN(decoder,
                              java::CastOpaquePtr<codec::OpusAudioStream>(ptr));
        auto checkpoint = decoder->CaptureCheckpoint();
        if (!checkpoint.ok()) {
            ThrowCheckpointContract(env, checkpoint.status());
            return checkpoint.status();
        }
        const std::shared_ptr<codec::OpusAudioCheckpoint> owner(
            std::move(*checkpoint));
        return java::ShareOpaquePtr(owner);
    } catch (const std::bad_alloc&) {
        ThrowOpusException(env, "java/lang/OutOfMemoryError"_sv,
                           "Failed to allocate Opus checkpoint"_sv);
        return jlong{0};
    }
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nCheckpointEmittedFrames(J)J",
    (ptr), jlong{-1}) {
    YSM_DECLARE_OR_RETURN(
        checkpoint, java::CastOpaquePtr<codec::OpusAudioCheckpoint>(ptr));
    return static_cast<jlong>(checkpoint->EmittedFrames());
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nRestoreCheckpoint(J)J",
              (ptr)) {
    restore_count.fetch_add(1, std::memory_order_relaxed);
    if (fail_next_restore.exchange(false, std::memory_order_relaxed)) {
        ThrowOpusException(env, "java/lang/IllegalStateException"_sv,
                           "Injected Opus checkpoint restore failure"_sv);
        return jlong{0};
    }
    try {
        const auto checkpoint =
            java::CastOpaquePtr<codec::OpusAudioCheckpoint>(ptr);
        if (!checkpoint.ok()) {
            ThrowCheckpointContract(env, checkpoint.status());
            return checkpoint.status();
        }
        if (pause_next_restore.exchange(false, std::memory_order_acq_rel)) {
            PauseRestore();
        }
        return java::MakeOpaquePtr<codec::OpusAudioStream>(
            *checkpoint->get());
    } catch (const std::bad_alloc&) {
        ThrowOpusException(env, "java/lang/OutOfMemoryError"_sv,
                           "Failed to restore Opus checkpoint"_sv);
        return jlong{0};
    }
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nCreateCount()J", ()) {
    return create_count.load(std::memory_order_relaxed);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nRestoreCount()J", ()) {
    return restore_count.load(std::memory_order_relaxed);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nFailNextRestore()V", ()) {
    fail_next_restore.store(true, std::memory_order_relaxed);
    return OkStatus();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nPauseNextRestore()V", ()) {
    auto& gate = GetRestoreGate();
    std::lock_guard lock(gate.mutex);
    YSM_ASSERT(!restore_paused.load(std::memory_order_acquire),
               absl::FailedPreconditionError("Opus restore is already paused"));
    gate.released = false;
    pause_next_restore.store(true, std::memory_order_release);
    return OkStatus();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nResumeRestore()V", ()) {
    auto& gate = GetRestoreGate();
    {
        std::lock_guard lock(gate.mutex);
        pause_next_restore.store(false, std::memory_order_release);
        gate.released = true;
    }
    gate.resumed.notify_all();
    return OkStatus();
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nFeed(JLjava/nio/ByteBuffer;)I",
    (ptr, input), jint{-1}) {
    YSM_ASSERT(ptr != 0 && input != nullptr,
               absl::InvalidArgumentError("Null Opus decoder input"sv));
    YSM_DECLARE_OR_RETURN(bytes, java::GetDirectBuffer(env, input));
    YSM_DECLARE_OR_RETURN(decoder,
                          java::CastOpaquePtr<codec::OpusAudioStream>(ptr));
    YSM_DECLARE_OR_RETURN(result, decoder->Feed(bytes));
    YSM_ASSERT(result.consumed <=
                   static_cast<std::size_t>((std::numeric_limits<jint>::max)()),
               absl::ResourceExhaustedError("Opus feed consumed too much"sv));
    return static_cast<jint>(result.consumed);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nEndInput(J)V",
              (ptr)) {
    YSM_ASSERT(ptr != 0, absl::InvalidArgumentError("Null Opus decoder"sv));
    YSM_DECLARE_OR_RETURN(decoder,
                          java::CastOpaquePtr<codec::OpusAudioStream>(ptr));
    decoder->EndInput();
    return OkStatus();
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/sound/OpusDecoder;nDecode(JLjava/nio/"
    "ByteBuffer;)I",
    (ptr, output), codec::OpusAudioStream::kError) {
    YSM_ASSERT(ptr != 0 && output != nullptr,
               absl::InvalidArgumentError("Null Opus decoder output"sv));
    YSM_DECLARE_OR_RETURN(bytes, java::GetDirectBuffer(env, output));
    YSM_DECLARE_OR_RETURN(decoder,
                          java::CastOpaquePtr<codec::OpusAudioStream>(ptr));
    return decoder->Decode(bytes);
}
}  // namespace ysm::lib::sound
