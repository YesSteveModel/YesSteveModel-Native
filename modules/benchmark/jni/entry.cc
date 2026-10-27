#include <array>
#include <cstdlib>

#include <mimalloc.h>
#include <java/entry.h>
#include <log.h>

namespace ysm {
namespace {
void JNICALL RawVoid(JNIEnv_*, jclass) {}

jint JNICALL RawIdentity(JNIEnv_*, jclass, jint value) {
    return value;
}

jlong JNICALL Malloc(JNIEnv_*, jclass, jint bytes) {
    return reinterpret_cast<jlong>(std::malloc(static_cast<std::size_t>(bytes)));
}

void JNICALL Free(JNIEnv_*, jclass, jlong address) {
    std::free(reinterpret_cast<void*>(address));
}

jlong JNICALL MiMalloc(JNIEnv_*, jclass, jint bytes) {
    return reinterpret_cast<jlong>(mi_malloc(static_cast<std::size_t>(bytes)));
}

void JNICALL MiFree(JNIEnv_*, jclass, jlong address) {
    mi_free(reinterpret_cast<void*>(address));
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/benchmark/JniEntryBenchmark;nEntryVoid()V",
              ()) {
    return absl::OkStatus();
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/benchmark/JniEntryBenchmark;nEntryIdentity(I)I",
    (value)) {
    return value;
}
}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    InitializeLogging();
    JNIEnv_* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) !=
            JNI_OK ||
        env == nullptr) {
        return JNI_ERR;
    }
    if (auto status = java::BindEntry(env); !status.ok()) {
        YSM_LOG(ERROR, "Failed to bind JNI benchmark entries: {}",
                status.message());
        return JNI_ERR;
    }

    auto clazz = env->FindClass("com/elfmcys/ysm/benchmark/JniEntryBenchmark");
    if (clazz == nullptr) {
        return JNI_ERR;
    }
    const std::array methods{
        JNINativeMethod{const_cast<char*>("nRawVoid"),
                        const_cast<char*>("()V"),
                        reinterpret_cast<void*>(&RawVoid)},
        JNINativeMethod{const_cast<char*>("nRawIdentity"),
                        const_cast<char*>("(I)I"),
                        reinterpret_cast<void*>(&RawIdentity)},
    };
    const auto result = env->RegisterNatives(
        clazz, methods.data(), static_cast<jint>(methods.size()));
    env->DeleteLocalRef(clazz);
    if (result != JNI_OK) {
        return JNI_ERR;
    }

    auto allocation_class = env->FindClass(
        "com/elfmcys/ysm/benchmark/MemoryAllocationBenchmark");
    if (allocation_class == nullptr) {
        return JNI_ERR;
    }
    const std::array allocation_methods{
        JNINativeMethod{const_cast<char*>("nMalloc"),
                        const_cast<char*>("(I)J"),
                        reinterpret_cast<void*>(&Malloc)},
        JNINativeMethod{const_cast<char*>("nFree"),
                        const_cast<char*>("(J)V"),
                        reinterpret_cast<void*>(&Free)},
        JNINativeMethod{const_cast<char*>("nMiMalloc"),
                        const_cast<char*>("(I)J"),
                        reinterpret_cast<void*>(&MiMalloc)},
        JNINativeMethod{const_cast<char*>("nMiFree"),
                        const_cast<char*>("(J)V"),
                        reinterpret_cast<void*>(&MiFree)},
    };
    const auto allocation_result = env->RegisterNatives(
        allocation_class, allocation_methods.data(),
        static_cast<jint>(allocation_methods.size()));
    env->DeleteLocalRef(allocation_class);
    return allocation_result == JNI_OK ? JNI_VERSION_1_8 : JNI_ERR;
}
}  // namespace ysm
