#include <absl/status/status.h>

#include <cstdint>

#include <java/buffer.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>

#include <legacy/v3.h>

#include "result_bridge.h"
#include "result_owner.h"

namespace ysm::legacy::java {
namespace {
absl::StatusOr<jobject> Import(JNIEnv_* env, jobject source,
                               jlong source_flags) {
    if (source == nullptr) {
        return NewFailureResult(
            env, absl::InvalidArgumentError("Legacy source buffer is null"));
    }
    YSM_DECLARE_OR_RETURN(
        bytes, ::ysm::java::BufferInput<true>::Get(env, source, source_flags));
    auto result = ::ysm::legacy::v3::Import(bytes);
    if (!result.ok()) {
        return NewFailureResult(env, result.status());
    }
    return NewSuccessResult(env, std::move(result).value());
}
}  // namespace

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/legacy/NativeLegacyImporter;nImport(Ljava/lang/"
    "Object;J)Lcom/elfmcys/ysm/natives/legacy/NativeLegacyImportResult$Output;",
    (source, source_flags)) {
    return Import(env, source, source_flags);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/legacy/NativeLegacyImporter;nShare(J)J",
              (handle)) {
    YSM_ASSERT(handle != 0,
               absl::InvalidArgumentError("Legacy result handle is zero"));
    return ::ysm::java::ShareOpaquePtr(handle);
}
}  // namespace ysm::legacy::java
