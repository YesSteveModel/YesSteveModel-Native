#include <array>
#include <cstdint>
#include <span>

#include <bitpack.h>
#include <java/array.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>
#include <gfx/renderer/model_state.h>
#include <physics/physics_world.h>
#include "err.h"
#include "scope_guard.h"

namespace ysm::lib::render {
namespace {
YSM_BIT_PACK(PackedCountPair, jlong,
             YSM_BIT_FIELD(low, uint32_t, 32),
             YSM_BIT_FIELD(high, uint32_t, 32));

absl::Status StageExtractOutput(JNIEnv_* env, gfx::renderer::ModelState& state,
                                const gfx::renderer::ModelState::ExtractOutput& extracted,
                                jshortArray locators, std::span<jlong, 4> slots) {
    if (extracted.locator_count != 0) {
        const auto indices = state.StagedLocatorBoneIndices(extracted.locator_count);
        env->SetShortArrayRegion(locators, 0, static_cast<jsize>(indices.size()),
                                 reinterpret_cast<const jshort*>(indices.data()));
    }
    YSM_ASSERT(!env->ExceptionCheck(),
               absl::InternalError("Failed to publish locator indices."));
    const auto poses = state.PoseView();
    YSM_DECLARE_OR_RETURN(render_counts, PackedCountPair::pack(
        poses.render_bone_indices.size(), extracted.locator_count));
    YSM_DECLARE_OR_RETURN(vertex_counts, PackedCountPair::pack(
        extracted.vertex_count, state.Schedule().translucent_vertex_count));
    slots[0] = reinterpret_cast<jlong>(poses.bone_poses.data());
    slots[1] = reinterpret_cast<jlong>(poses.render_bone_indices.data());
    slots[2] = render_counts;
    slots[3] = vertex_counts;
    return OkStatus();
}

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
    }
    YSM_ASSERT(output.locator_count <= static_cast<size_t>(locator_capacity),
               absl::InternalError("Invalid locator count."));
    std::array<jlong, 4> packed_output;
    YSM_RETURN_IF_ERROR(StageExtractOutput(env, *state, output, locator_bone_indices_array, packed_output));
    YSM_RETURN_IF_ERROR(java::WriteLongArray<true>(env, output_array, packed_output));
    YSM_ASSERT(!env->ExceptionCheck(),
               absl::InternalError("Failed to publish ModelState output."));
    invalidate.Release();
    return OkStatus();
}

enum StepMode : jint { kObserve, kReset, kStep };
constexpr uint32_t kExtractOk = 1;
constexpr uint32_t kPhysicsAccepted = 2;

absl::StatusOr<gfx::renderer::ModelState::ExtractOutput> ExtractPhysicsPose(
    JNIEnv_* env, jfloatArray attributes, gfx::renderer::ModelState& state,
    const std::shared_ptr<gfx::bake::BakedModel>& model, size_t bone_count,
    jsize attribute_count, size_t locator_capacity, const physics::PhysicsWorld& world) {
    const auto deltas = world.HasSample() ? world.ActiveDeltas() :
        std::span<const gfx::renderer::BonePhysicsDelta>{};
    YSM_DECLARE_OR_RETURN(borrow, java::CriticalFloatArray<true>::PinValidatedRange(env, attributes, 0, attribute_count));
    const auto* bones = reinterpret_cast<const gfx::renderer::BoneAttribute*>(borrow.data());
    return magic_enum::enum_switch([&](auto simd_type) {
        return state.Extract(simd::Tag<simd_type>{}, model, {bones, bone_count},
                             locator_capacity, deltas);
    }, simd::kSupported);
}

bool GatherPhysicsPose(JNIEnv_* env, jfloatArray attributes, jsize attribute_count, physics::PhysicsWorld& world) {
    auto borrow = java::CriticalFloatArray<true>::PinValidatedRange(env, attributes, 0, attribute_count);
    if (!borrow.ok()) [[unlikely]] {
        return false;
    }
    world.GatherBoneAttributes(borrow->span());
    return true;
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

YSM_JNI_HOT_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeModelState;nExtractWithPhysics(JJJ[F[S[JIFDDDFFFF)I",
    (state_ptr, baked_ptr, world_ptr, attributes, locators, output,
     mode, dt, root_x, root_y, root_z, quat_x, quat_y, quat_z, quat_w)) {
    uint32_t flags = 0;
    auto* state_handle = java::TryCastOpaquePtr<gfx::renderer::ModelState>(state_ptr);
    if (!state_handle) [[unlikely]] {
        return 0;
    }
    auto& state = **state_handle;
    state.Invalidate();
    auto* model = java::TryCastOpaquePtr<gfx::bake::BakedModel>(baked_ptr);
    auto* world_handle = java::TryCastOpaquePtr<physics::PhysicsWorld>(world_ptr);
    if (!model || !world_handle || !attributes || !locators || !output ||
        mode < kObserve || mode > kStep) [[unlikely]] {
        return 0;
    }

    const size_t bone_count = (*model)->Bones().list.size();
    const auto locator_capacity = static_cast<size_t>(env->GetArrayLength(locators));
    const auto attribute_count = env->GetArrayLength(attributes);
    if (static_cast<size_t>(attribute_count) != bone_count * gfx::renderer::BoneAttribute::kFloatCount ||
        locator_capacity < bone_count || env->GetArrayLength(output) < 5) [[unlikely]] {
        return 0;
    }

    auto& world = **world_handle;
    std::array<jlong, 5> slots{};
    bool published = false;
    try {
        bool gathered = true;
        if (mode != kObserve) {
            physics::StepResult result{physics::StepOutcome::kScratchExhausted};
            if (world.PrepareGather()) {
                gathered = GatherPhysicsPose(env, attributes, attribute_count, world);
                if (gathered) {
                    result = world.AdvanceGathered(mode == kReset, dt, {root_x, root_y, root_z},
                                                   {quat_x, quat_y, quat_z, quat_w});
                }
            }
            if (gathered) {
                flags = kPhysicsAccepted | (static_cast<uint32_t>(result.outcome) << 2);
                slots[4] = result.engine_error;
            }
        }
        if (gathered) {
            auto extracted = ExtractPhysicsPose(env, attributes, state, *model, bone_count, attribute_count,
                                                locator_capacity, world);
            published = extracted.ok() && StageExtractOutput(
                env, state, *extracted, locators, std::span(slots).first<4>()).ok();
        }
    } catch (...) {
        published = false;
    }
    if (env->ExceptionCheck()) [[unlikely]] {
        env->ExceptionClear();
        published = false;
    }
    env->SetLongArrayRegion(output, 0, static_cast<jsize>(slots.size()), slots.data());
    if (env->ExceptionCheck()) [[unlikely]] {
        env->ExceptionClear();
        published = false;
    }
    if (published) {
        flags |= kExtractOk;
    } else {
        state.Invalidate();
    }
    return static_cast<jint>(flags);
}
}  // namespace ysm::lib::render
