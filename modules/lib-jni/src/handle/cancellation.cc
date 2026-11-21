#include <java/entry.h>
#include <java/opaque_ptr.h>

#include "cancellation.h"

namespace ysm::lib::handle {
YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeCancellation;nCreate()J", (),
              jlong{0}) {
    return java::MakeOpaquePtr<CancellationState>();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeCancellation;nRequest(J)Z",
              (handle)) {
    YSM_DECLARE_OR_RETURN(state,
                          java::AcquireOpaquePtr<CancellationState>(handle));
    state->Request();
    return OkStatus();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeCancellation;nQuery(J)I",
              (handle), jint{-1}) {
    YSM_DECLARE_OR_RETURN(state,
                          java::AcquireOpaquePtr<CancellationState>(handle));
    return state->IsRequested() ? jint{1} : jint{0};
}
}  // namespace ysm::lib::handle
