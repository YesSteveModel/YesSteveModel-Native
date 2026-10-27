#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <ranges>
#include <cglm/vec4.h>

#include "err.h"
#include "fast_math.h"
#include "inline.h"
#include "profile.h"
#include "gfx/math/pose_stack.h"
#include "math/mat4_mul.h"
#include "render_state.h"

YSM_FAST_MATH_BEGIN

#ifndef YSM_RENDER_STATE_IMPL_H
#define YSM_RENDER_STATE_IMPL_H

namespace ysm::gfx::renderer {
namespace {
YSM_INLINE float Determinant3(float a0, float a1, float a2, float b0, float b1, float b2,
                   float c0, float c1, float c2) noexcept {
    return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) +
           a2 * (b0 * c1 - b1 * c0);
}

YSM_INLINE void ExtractRow(const mat4 matrix, size_t row, vec4 destination) noexcept {
    for (size_t column = 0; column < 4; ++column) {
        destination[column] = matrix[column][row];
    }
}

YSM_INLINE void ComputeFacingCoefficient(const mat4 matrix, vec4 destination) noexcept {
    vec4 x;
    vec4 y;
    vec4 w;
    ExtractRow(matrix, 0, x);
    ExtractRow(matrix, 1, y);
    ExtractRow(matrix, 3, w);
    destination[0] =
        Determinant3(x[1], x[2], x[3], y[1], y[2], y[3], w[1], w[2], w[3]);
    destination[1] =
        -Determinant3(x[0], x[2], x[3], y[0], y[2], y[3], w[0], w[2], w[3]);
    destination[2] =
        Determinant3(x[0], x[1], x[3], y[0], y[1], y[3], w[0], w[1], w[3]);
    destination[3] =
        -Determinant3(x[0], x[1], x[2], y[0], y[1], y[2], w[0], w[1], w[2]);
}

YSM_INLINE void CombineFacingCoefficient(const vec4 outer, const BonePose& bone,
                              vec4 destination) noexcept {
    vec3 translated;
    glm_vec3_scale(bone.pose[3], outer[3], translated);
    glm_vec3_sub(outer, translated, translated);

    for (size_t column = 0; column < 3; ++column) {
        destination[column] = bone.tangent_orientation *
                              glm_vec3_dot(translated, bone.normal[column]);
    }
    destination[3] = bone.tangent_orientation * bone.normal_scale * outer[3];
}

YSM_INLINE bool IsFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(
        values, [](float value) { return std::isfinite(value); });
}

YSM_INLINE bool DetermineUniformScale(const mat3 linear) noexcept {
    const float length_squared[3]{
        glm_vec3_norm2(linear[0]),
        glm_vec3_norm2(linear[1]),
        glm_vec3_norm2(linear[2]),
    };
    const auto maximum_length_squared =
        std::max({length_squared[0], length_squared[1], length_squared[2]});
    if (!(maximum_length_squared > 0.0f) ||
        !std::isfinite(maximum_length_squared)) {
        return false;
        }

    constexpr float kRelativeTolerance =
        32.0f * std::numeric_limits<float>::epsilon();
    const auto tolerance = maximum_length_squared * kRelativeTolerance;
    for (size_t column = 1; column < 3; ++column) {
        if (std::abs(length_squared[column] - length_squared[0]) > tolerance) {
            return false;
        }
    }
    if (std::abs(glm_vec3_dot(linear[0], linear[1])) > tolerance ||
        std::abs(glm_vec3_dot(linear[0], linear[2])) > tolerance ||
        std::abs(glm_vec3_dot(linear[1], linear[2])) > tolerance) {
        return false;
        }
    return true;
}

YSM_INLINE uint8_t MultiplyChannel(uint8_t parent, uint8_t value) noexcept {
    return static_cast<uint8_t>((static_cast<uint32_t>(parent) * value +
                                 127u) /
                                255u);
}
}  // namespace
}

#endif  //YSM_RENDER_STATE_IMPL_H

namespace ysm::gfx::renderer {
template <>
YSM_SIMD_TARGET
absl::Status RenderState::UpdateCommon<YSM_SIMD_TAG>(YSM_SIMD_TAG tag, const RenderParameters& params,
                                       const ModelState& model_state) {
    YSM_PROFILE_ZONE("YSM/C++/RenderState.UpdateCommon");
    YSM_ASSERT(IsFinite(std::span(&params.model[0][0], 16)) &&
                   IsFinite(std::span(&params.view[0][0], 16)) &&
                   IsFinite(std::span(&params.projection[0][0], 16)) &&
                   IsFinite(std::span(&params.normal[0][0], 9)),
               absl::InvalidArgumentError("Non-finite render matrix."));
    auto& model_info = model_state.Model()->Info();
    auto& bone_info = model_state.Model()->Bones();
    auto& schedule = model_state.Schedule();
    YSM_PROFILE_VALUE(bone_info.list.size());

    if (schedule.mode == RenderSchedulingMode::kWorkerReadySpin) {
        worker_ready_size = schedule.tasks.size();
        if (worker_ready_flags.size() < worker_ready_size) {
            worker_ready_flags =
                std::remove_cvref_t<decltype(worker_ready_flags)>(worker_ready_size);
        } else {
            for (size_t worker_index = 0; worker_index < worker_ready_size;
                 ++worker_index) {
                worker_ready_flags[worker_index].value.store(
                    false, std::memory_order_relaxed);
            }
        }
    } else {
        worker_ready_size = 0;
    }

    mat3 liner;
    glm_mat4_pick3(params.model, liner);
    outer_uniform_scale = DetermineUniformScale(liner);
    outer_tangent_orientation = 1.0f;
    if (model_info.has_pbr) {
        const auto model_determinant = glm_mat3_det(liner);
        if (std::isfinite(model_determinant) && model_determinant < 0.0f) {
            outer_tangent_orientation = -1.0f;
        }
    }

    if (bone_states.size() < bone_info.list.size()) {
        bone_states.resize(bone_info.list.size());
    }

    mat4 view_model;
    math::AffineMul(tag, params.view, params.model, view_model);
    math::Mat4Mul(tag, params.projection, view_model, clip_model);
    YSM_ASSERT(IsFinite(std::span(&view_model[0][0], 16)) &&
                   IsFinite(std::span(&clip_model[0][0], 16)),
               absl::InvalidArgumentError(
                   "Render matrix composition is non-finite."));
    ComputeFacingCoefficient(clip_model, clip_facing);

    return OkStatus();
}

template <>
YSM_SIMD_TARGET
RenderResult RenderState::Update<YSM_SIMD_TAG>(YSM_SIMD_TAG tag,
                                 const RenderParameters& params,
                                 const ModelState& model_state,
                                 uint32_t worker_index,
                                 uint32_t worker_count) {
    YSM_PROFILE_ZONE("YSM/C++/RenderState.Update");
    auto& bone_info = model_state.Model()->Bones();
    auto model_pose = model_state.PoseView();
    auto [begin, end] = DetermineTaskRange(worker_index,
        worker_count, model_pose.render_bone_indices.size());
    YSM_PROFILE_VALUE(end - begin);

    auto result = RenderResult::kSuccess;
    for (auto i = begin; i < end; ++i) {
        const auto bone_index = model_pose.render_bone_indices[i];
        if (bone_index >= bone_info.list.size()) [[unlikely]] {
            return RenderResult::kStateInvalid;
        }

        const auto& source = model_pose.bone_poses[bone_index];
        auto& destination = bone_states[bone_index];

        destination.result = RenderResult::kSuccess;

        math::AffineMul(tag, params.model, source.pose,
                        destination.pose);
        glm_mat3_mul(params.normal, source.normal, destination.normal);
        destination.uniform_scale =
            outer_uniform_scale && source.uniform_scale;
        destination.tangent_orientation =
                outer_tangent_orientation * source.tangent_orientation;

        CombineFacingCoefficient(clip_facing, source,
                                 destination.facing_coeff);

        if (!(IsFinite(std::span(&destination.pose[0][0], 16)) &&
                IsFinite(std::span(&destination.normal[0][0], 9)) &&
                IsFinite(std::span(destination.facing_coeff, 4)))) [[unlikely]] {
            destination.result = RenderResult::kOutputRejected;
        }
        if (!bone_info.list[bone_index].solid) {
            mat4 clip;
            math::Mat4Mul(tag, clip_model, source.pose, clip);
            if (!IsFinite(std::span(&clip[0][0], 16))) [[unlikely]] {
                destination.result = RenderResult::kOutputRejected;
            }
            ExtractRow(clip, 2, destination.depth_z);
            ExtractRow(clip, 3, destination.depth_w);
        }

        if (destination.result != RenderResult::kSuccess) [[unlikely]] {
            // Check the source only on failure: finite inputs can overflow together.
            if (!(IsFinite(std::span(&source.pose[0][0], 16)) &&
                  IsFinite(std::span(&source.normal[0][0], 9)) &&
                  std::isfinite(source.tangent_orientation) &&
                  std::isfinite(source.normal_scale))) {
                destination.result = RenderResult::kStateInvalid;
            }
            if (result != RenderResult::kStateInvalid) {
                result = destination.result;
            }
        }

        // 这对吗？
        Color opaque_color;
        opaque_color.components.r = MultiplyChannel(
            params.color.components.r, source.color.components.r);
        opaque_color.components.g = MultiplyChannel(
            params.color.components.g, source.color.components.g);
        opaque_color.components.b = MultiplyChannel(
            params.color.components.b, source.color.components.b);
        opaque_color.components.a = params.color.components.a;
        auto translucent_color = opaque_color;
        translucent_color.components.a = MultiplyChannel(
            params.color.components.a, source.color.components.a);
        destination.packed_opaque_color = opaque_color.packed;
        destination.packed_translucent_color = translucent_color.packed;

        destination.packed_light = source.glowing == 0xFFu
                                       ? params.light
                                       : (static_cast<uint32_t>(source.glowing)
                                              << 4) |
                                             (static_cast<uint32_t>(source.glowing)
                                              << 20);
    }
    return result;
}
}

YSM_FAST_MATH_END
