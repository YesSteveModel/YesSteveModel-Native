#include "bridge.h"

#include <java/entry.h>
#include <storage/file_id_allocator.h>

namespace ysm::lib::storage_bridge {

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeFileIdAllocator;nOpen(Ljava/lang/String;J)J", (root, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(path, PathInput(env, root));
    return Complete(env, [&] { return storage::FileIdAllocator::Open(path, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeFileIdAllocator;nAllocate(JJ)J", (handle, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::FileIdAllocator>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return Complete(env, [&] { return owner->Allocate(*token); });
}

}  // namespace ysm::lib::storage_bridge
