#include <array>
#include <cstdint>
#include <span>

#include <bitpack.h>
#include <java/array.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>
#include <gfx/renderer/model_state.h>
#include "err.h"
#include "scope_guard.h"

namespace ysm::lib::render {
namespace {
YSM_BIT_PACK(PackedCountPair, jlong,
             YSM_BIT_FIELD(low, uint32_t, 32),
             YSM_BIT_FIELD(high, uint32_t, 32));

template <bool kLocal>
absl::Status Extract(JNIEnv_* env, jlong state_ptr, jlong baked_model_ptr,
                     jfloatArray bone_attributes_array,
                     jshortArray locator_bone_indices_array,
                     jlongArray output_array, jint root_index = 0) {
    YSM_DECLARE_OR_RETURN(
        state, java::CastOpaquePtr<gfx::renderer::ModelState>(state_ptr));
    state->Invalidate();
    auto invalidate = ScopeGuard([&] { state->Invalidate(); });
    YSM_DECLARE_OR_RETURN(
        baked_model,
        java::CastOpaquePtr<gfx::bake::BakedModel>(baked_model_ptr));
    YSM_ASSERT(env != nullptr && locator_bone_indices_array != nullptr &&
                   output_array != nullptr,
               absl::InvalidArgumentError("Invalid ModelState output."));
    if constexpr (kLocal) {
        YSM_ASSERT(root_index >= 0,
                   absl::InvalidArgumentError("Invalid subtree root index."));
    }
    const auto locator_capacity =
        env->GetArrayLength(locator_bone_indices_array);
    gfx::renderer::ModelState::ExtractOutput output;
    size_t locator_count;
    {
        YSM_DECLARE_OR_RETURN(
            bone_attributes,
            java::CriticalFloatArray<true>::Get(env, bone_attributes_array));
        YSM_ASSERT(
            bone_attributes.size() %
                    gfx::renderer::BoneAttribute::kFloatCount ==
                0,
            absl::InvalidArgumentError(
                "Invalid bone attribute buffer size."));
        const auto* attributes =
            reinterpret_cast<const gfx::renderer::BoneAttribute*>(
                bone_attributes.data());
        const auto attribute_count =
            bone_attributes.size() / gfx::renderer::BoneAttribute::kFloatCount;
        YSM_DECLARE_OR_RETURN(
            extracted,
            magic_enum::enum_switch(
                [&](auto kSimdType) ->
                    absl::StatusOr<gfx::renderer::ModelState::ExtractOutput> {
                    static constexpr simd::Tag<kSimdType> tag;
                    if constexpr (kLocal) {
                        return state->ExtractSubtree(
                            tag, baked_model, {attributes, attribute_count},
                            static_cast<size_t>(locator_capacity),
                            static_cast<size_t>(root_index));
                    } else {
                        return state->Extract(
                            tag, baked_model, {attributes, attribute_count},
                            static_cast<size_t>(locator_capacity));
                    }
                },
                simd::kSupported));
        output = extracted;
        locator_count = extracted.locator_count;
    }
    YSM_ASSERT(locator_count <= static_cast<size_t>(locator_capacity),
               absl::InternalError("Invalid locator count."));
    YSM_RETURN_IF_ERROR(java::WriteShortArray<true>(
        env, locator_bone_indices_array,
        state->StagedLocatorBoneIndices(locator_count)));
    YSM_ASSERT(!env->ExceptionCheck(),
               absl::InternalError("Failed to publish locator indices."));
    const auto pose_view = state->PoseView();
    const auto bone_poses = pose_view.bone_poses;
    const auto render_bone_indices = pose_view.render_bone_indices;
    YSM_DECLARE_OR_RETURN(render_counts,
                          PackedCountPair::pack(render_bone_indices.size(),
                                                output.locator_count));
    YSM_DECLARE_OR_RETURN(
        vertex_counts,
        PackedCountPair::pack(output.vertex_count,
                              state->Schedule().translucent_vertex_count));
    const std::array<jlong, 4> packed_output{
        reinterpret_cast<jlong>(bone_poses.data()),
        reinterpret_cast<jlong>(render_bone_indices.data()),
        render_counts,
        vertex_counts,
    };
    YSM_RETURN_IF_ERROR(java::WriteLongArray<true>(env, output_array, packed_output));
    YSM_ASSERT(!env->ExceptionCheck(),
               absl::InternalError("Failed to publish ModelState output."));
    invalidate.Release();
    return OkStatus();
}
}  // namespace

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeModelState;nCreate()J",
    ()) {
    return java::MakeOpaquePtr<gfx::renderer::ModelState>();
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeModelState;nInvalidate(J)V",
    (state_ptr)) {
    YSM_DECLARE_OR_RETURN(
        state, java::CastOpaquePtr<gfx::renderer::ModelState>(state_ptr));
    state->Invalidate();
    return OkStatus();
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeModelState;nExtract(JJ[F[S[J)Z",
    (state_ptr, baked_model_ptr, bone_attributes_array,
     locator_bone_indices_array, output_array)) {
    return Extract<false>(env, state_ptr, baked_model_ptr, bone_attributes_array,
                          locator_bone_indices_array, output_array);
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeModelState;nExtractSubtree(JJ[F[S[JI)Z",
    (state_ptr, baked_model_ptr, bone_attributes_array,
     locator_bone_indices_array, output_array, root_index)) {
    return Extract<true>(env, state_ptr, baked_model_ptr, bone_attributes_array,
                         locator_bone_indices_array, output_array, root_index);
}
}  // namespace ysm::lib::render
