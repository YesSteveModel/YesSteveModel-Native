#pragma once

#include <c_string_view.h>
#include <gfx/bake/baked_model.h>
#include <java/array.h>
#include <java/opaque_ptr.h>

namespace ysm::lib::bake {
inline absl::StatusOr<jlong> DeliverModel(
    JNIEnv_* env, std::unique_ptr<gfx::bake::BakedModel> model,
    jshortArray sorted_bone_indices_array) {
    if (env->ExceptionCheck()) {
        return absl::CancelledError("Model delivery has a pending exception."_sv);
    }
    YSM_RETURN_IF_ERROR(java::WriteShortArray(
        env, sorted_bone_indices_array, model->Bones().sorted_bone_indices));
    if (env->ExceptionCheck()) {
        return absl::CancelledError("Model mapping delivery failed."_sv);
    }
    return java::MakeOpaquePtr<gfx::bake::BakedModel>(std::move(*model));
}
}  // namespace ysm::lib::bake
