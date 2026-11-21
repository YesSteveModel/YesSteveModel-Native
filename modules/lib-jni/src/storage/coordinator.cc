#include "bridge.h"

#include <java/entry.h>
#include <storage/coordinator.h>

namespace ysm::lib::storage_bridge {

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCoordination;nOpen(Ljava/lang/String;J)J", (scope, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(path, PathInput(env, scope));
    return Complete(env, [&] { return storage::Coordinator::Open(path, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCoordination;nBeginCleanup(JJ)[J", (handle, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::Coordinator>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteSimple(env, [&] { return owner->BeginCleanup(*token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCoordination;nEndCleanup(J)[J", (handle)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::Coordinator>(handle));
    return CompleteSimple(env, [&] { return owner->EndCleanup(); });
}

}  // namespace ysm::lib::storage_bridge
