#include <mimalloc.h>
#include <bit>

#include <java/buffer.h>
#include <java/entry.h>

namespace ysm::lib::buffer {
static_assert(sizeof(jlong) == sizeof(void*));

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/buffer/NativeHeapBuffer;nAlloc(II)Ljava/nio/ByteBuffer;",
    (size, alignment)) {
    void* ptr;
    if (alignment <= 0) {
        ptr = mi_malloc(size);
    } else {
        ptr = mi_aligned_alloc(alignment, size);
    }
    if (ptr == nullptr) {
        return static_cast<jobject>(nullptr);
    }
    auto buffer = env->NewDirectByteBuffer(ptr, size);
    if (buffer == nullptr || env->ExceptionCheck()) {
        mi_free(ptr);
        if (buffer != nullptr) {
            env->DeleteLocalRef(buffer);
        }
        return static_cast<jobject>(nullptr);
    }
    return buffer;
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/buffer/NativeHeapBuffer;nFree(J)V",
    (addr)) {
    mi_free(reinterpret_cast<void*>(addr));
    return OkStatus();
}
}  // namespace ysm::lib::buffer
