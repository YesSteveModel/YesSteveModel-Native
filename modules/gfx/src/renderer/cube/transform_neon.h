#pragma once

#include <arm_neon.h>
#include <cstring>
#include <limits>

#include "cpu.h"
#include "fast_math.h"
#include "inline.h"

#include "gfx/bake/baked_model.h"
#include "renderer/render_state.h"
#include "output.h"
#include "transform_common.h"

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::renderer::cube {

namespace internal {

struct NeonVec3 {
    float32x4_t x;
    float32x4_t y;
    float32x4_t z;
};

struct NeonVec2u {
    uint32x4_t first;
    uint32x4_t second;
};

YSM_INLINE float32x4_t InverseSqrtNeon(float32x4_t value) noexcept {
#if YSM_TRANSFORM_USE_APPROXIMATE_RSQRT
    return vrsqrteq_f32(value);
#else
    return vdivq_f32(vdupq_n_f32(1.0f), vsqrtq_f32(value));
#endif
}

YSM_INLINE NeonVec3 TransformPointNeon(const mat4 matrix, float32x4_t x,
                                       float32x4_t y,
                                       float32x4_t z) noexcept {
    const auto column0 = vld1q_f32(matrix[0]);
    const auto column1 = vld1q_f32(matrix[1]);
    const auto column2 = vld1q_f32(matrix[2]);
    const auto column3 = vld1q_f32(matrix[3]);
    const auto x_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, column0, 0), y, column1, 0);
    const auto x_zt = vfmaq_laneq_f32(vdupq_laneq_f32(column3, 0), z,
                                      column2, 0);
    const auto y_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, column0, 1), y, column1, 1);
    const auto y_zt = vfmaq_laneq_f32(vdupq_laneq_f32(column3, 1), z,
                                      column2, 1);
    const auto z_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, column0, 2), y, column1, 2);
    const auto z_zt = vfmaq_laneq_f32(vdupq_laneq_f32(column3, 2), z,
                                      column2, 2);
    return {vaddq_f32(x_xy, x_zt), vaddq_f32(y_xy, y_zt),
            vaddq_f32(z_xy, z_zt)};
}

YSM_INLINE NeonVec3 TransformDirectionNeon(const mat3 matrix, float32x4_t x,
                                           float32x4_t y,
                                           float32x4_t z) noexcept {
    const auto first = vld1q_f32(matrix[0]);
    const auto second = vld1q_f32(&matrix[1][1]);
    const auto last = vld1q_dup_f32(&matrix[2][2]);
    const auto x_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, first, 0), y, first, 3);
    const auto x_z = vmulq_laneq_f32(z, second, 2);
    const auto y_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, first, 1), y, second, 0);
    const auto y_z = vmulq_laneq_f32(z, second, 3);
    const auto z_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, first, 2), y, second, 1);
    const auto z_z = vmulq_laneq_f32(z, last, 0);
    return {vaddq_f32(x_xy, x_z), vaddq_f32(y_xy, y_z),
            vaddq_f32(z_xy, z_z)};
}

YSM_INLINE NeonVec3 TransformDirectionNeon(const mat4 matrix, float32x4_t x,
                                           float32x4_t y,
                                           float32x4_t z) noexcept {
    const auto column0 = vld1q_f32(matrix[0]);
    const auto column1 = vld1q_f32(matrix[1]);
    const auto column2 = vld1q_f32(matrix[2]);
    const auto x_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, column0, 0), y, column1, 0);
    const auto x_z = vmulq_laneq_f32(z, column2, 0);
    const auto y_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, column0, 1), y, column1, 1);
    const auto y_z = vmulq_laneq_f32(z, column2, 1);
    const auto z_xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, column0, 2), y, column1, 2);
    const auto z_z = vmulq_laneq_f32(z, column2, 2);
    return {vaddq_f32(x_xy, x_z), vaddq_f32(y_xy, y_z),
            vaddq_f32(z_xy, z_z)};
}

YSM_INLINE float32x4_t DotNeon(float32x4_t x, float32x4_t y,
                               float32x4_t z, float32x4_t w,
                               const vec4 coefficients) noexcept {
    const auto values = vld1q_f32(coefficients);
    const auto xy =
        vfmaq_laneq_f32(vmulq_laneq_f32(x, values, 0), y, values, 1);
    const auto zw =
        vfmaq_laneq_f32(vmulq_laneq_f32(z, values, 2), w, values, 3);
    return vaddq_f32(xy, zw);
}

YSM_INLINE uint32x4_t DirectionValidityNeon(
    float32x4_t length_squared) noexcept {
    return vandq_u32(
        vcgtq_f32(length_squared, vdupq_n_f32(0.0f)),
        vcleq_f32(length_squared,
                  vdupq_n_f32(std::numeric_limits<float>::max())));
}

YSM_INLINE void NormalizeDirectionNeon(NeonVec3& direction) noexcept {
    const auto length_xy =
        vfmaq_f32(vmulq_f32(direction.x, direction.x), direction.y,
                  direction.y);
    const auto length_z = vmulq_f32(direction.z, direction.z);
    const auto length_squared = vaddq_f32(length_xy, length_z);
    const auto valid = DirectionValidityNeon(length_squared);
    const auto one = vdupq_n_f32(1.0f);
    const auto safe_length_squared =
        vbslq_f32(valid, length_squared, one);
    const auto inverse_length = InverseSqrtNeon(safe_length_squared);
    direction.x = vreinterpretq_f32_u32(
        vandq_u32(valid, vreinterpretq_u32_f32(
                              vmulq_f32(direction.x, inverse_length))));
    direction.y = vreinterpretq_f32_u32(
        vandq_u32(valid, vreinterpretq_u32_f32(
                              vmulq_f32(direction.y, inverse_length))));
    direction.z = vreinterpretq_f32_u32(
        vandq_u32(valid, vreinterpretq_u32_f32(
                              vmulq_f32(direction.z, inverse_length))));
}

YSM_INLINE void NormalizeDirectionsNeon(NeonVec3& first,
                                        NeonVec3& second) noexcept {
    const auto first_xy =
        vfmaq_f32(vmulq_f32(first.x, first.x), first.y, first.y);
    const auto second_xy =
        vfmaq_f32(vmulq_f32(second.x, second.x), second.y, second.y);
    const auto first_z = vmulq_f32(first.z, first.z);
    const auto second_z = vmulq_f32(second.z, second.z);
    const auto first_length_squared = vaddq_f32(first_xy, first_z);
    const auto second_length_squared = vaddq_f32(second_xy, second_z);
    const auto first_valid = DirectionValidityNeon(first_length_squared);
    const auto second_valid = DirectionValidityNeon(second_length_squared);
    const auto one = vdupq_n_f32(1.0f);
    const auto first_inverse = InverseSqrtNeon(
        vbslq_f32(first_valid, first_length_squared, one));
    const auto second_inverse = InverseSqrtNeon(
        vbslq_f32(second_valid, second_length_squared, one));
    first.x = vreinterpretq_f32_u32(
        vandq_u32(first_valid, vreinterpretq_u32_f32(
                                    vmulq_f32(first.x, first_inverse))));
    second.x = vreinterpretq_f32_u32(
        vandq_u32(second_valid, vreinterpretq_u32_f32(
                                     vmulq_f32(second.x, second_inverse))));
    first.y = vreinterpretq_f32_u32(
        vandq_u32(first_valid, vreinterpretq_u32_f32(
                                    vmulq_f32(first.y, first_inverse))));
    second.y = vreinterpretq_f32_u32(
        vandq_u32(second_valid, vreinterpretq_u32_f32(
                                     vmulq_f32(second.y, second_inverse))));
    first.z = vreinterpretq_f32_u32(
        vandq_u32(first_valid, vreinterpretq_u32_f32(
                                    vmulq_f32(first.z, first_inverse))));
    second.z = vreinterpretq_f32_u32(
        vandq_u32(second_valid, vreinterpretq_u32_f32(
                                     vmulq_f32(second.z, second_inverse))));
}

YSM_INLINE uint32x4_t ConvertSnorm8Neon(float32x4_t value) noexcept {
    auto result = vcvtnq_s32_f32(vmulq_n_f32(value, 127.0f));
    result = vmaxq_s32(vdupq_n_s32(-127),
                       vminq_s32(vdupq_n_s32(127), result));
    return vandq_u32(vreinterpretq_u32_s32(result), vdupq_n_u32(0xff));
}

YSM_INLINE NeonVec2u ConvertSnorm8PairNeon(float32x4_t first,
                                          float32x4_t second) noexcept {
    first = vmulq_n_f32(first, 127.0f);
    second = vmulq_n_f32(second, 127.0f);
    auto first_int = vcvtnq_s32_f32(first);
    auto second_int = vcvtnq_s32_f32(second);
    const auto lower = vdupq_n_s32(-127);
    first_int = vmaxq_s32(lower, first_int);
    second_int = vmaxq_s32(lower, second_int);
    const auto upper = vdupq_n_s32(127);
    first_int = vminq_s32(upper, first_int);
    second_int = vminq_s32(upper, second_int);
    const auto byte_mask = vdupq_n_u32(0xff);
    return {vandq_u32(vreinterpretq_u32_s32(first_int), byte_mask),
            vandq_u32(vreinterpretq_u32_s32(second_int), byte_mask)};
}

YSM_INLINE uint32x4_t PackSnorm4x8Neon(float32x4_t x, float32x4_t y,
                                       float32x4_t z,
                                       float32x4_t w) noexcept {
    const auto packed_xy_values = ConvertSnorm8PairNeon(x, y);
    const auto packed_xy =
        vorrq_u32(packed_xy_values.first,
                  vshlq_n_u32(packed_xy_values.second, 8));
    const auto packed_zw_values = ConvertSnorm8PairNeon(z, w);
    const auto packed_zw =
        vorrq_u32(vshlq_n_u32(packed_zw_values.first, 16),
                  vshlq_n_u32(packed_zw_values.second, 24));
    return vorrq_u32(packed_xy, packed_zw);
}

YSM_INLINE uint32x4_t PackSnorm3x8Neon(float32x4_t x, float32x4_t y,
                                       float32x4_t z) noexcept {
    const auto packed_xy_values = ConvertSnorm8PairNeon(x, y);
    const auto packed_xy =
        vorrq_u32(packed_xy_values.first,
                  vshlq_n_u32(packed_xy_values.second, 8));
    const auto packed_z = vshlq_n_u32(ConvertSnorm8Neon(z), 16);
    return vorrq_u32(packed_xy, packed_z);
}

YSM_INLINE void StoreBackFacesNeon(bool* destination,
                                   uint32x4_t back_face) noexcept {
    const auto values = vshrq_n_u32(back_face, 31);
    const auto packed16 = vmovn_u32(values);
    const auto packed8 =
        vmovn_u16(vcombine_u16(packed16, vdup_n_u16(0)));
    const auto packed_bytes =
        vget_lane_u32(vreinterpret_u32_u8(packed8), 0);
    std::memcpy(destination, &packed_bytes, sizeof(packed_bytes));
}

YSM_INLINE float32x4_t BackFaceSignNeon(uint32x4_t back_face) noexcept {
    return vreinterpretq_f32_u32(
        vandq_u32(back_face, vdupq_n_u32(0x80000000U)));
}

template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
YSM_INLINE void TransformSingleQuadNeon(
    const CubeGroupType& cube_group, const RenderBoneState& state,
    CubeOutput<CubeGroupType>& out) {
    const auto position_x = vld1q_f32(cube_group.pos[0].data());
    const auto position_y = vld1q_f32(cube_group.pos[1].data());
    const auto position_z = vld1q_f32(cube_group.pos[2].data());
    const auto position = TransformPointNeon(
        state.pose, position_x, position_y, position_z);
    vst1q_f32(out.pos[0].data(), position.x);
    vst1q_f32(out.pos[1].data(), position.y);
    vst1q_f32(out.pos[2].data(), position.z);

    TransformQuadAttributes<kCulling, kIris, kHasPbr, kPosOnly>(
        simd::Tag<simd::Type::NEON>{}, cube_group, state, out, 0);
}

template <uint32_t kCubeCount, bool kCulling, bool kIris, bool kHasPbr,
          bool kPosOnly, typename CubeGroupType>
YSM_INLINE void TransformCubesNeon(const CubeGroupType& cube_group,
                                   const RenderBoneState& state,
                                   CubeOutput<CubeGroupType>& out) {
    constexpr uint32_t kPositionCount = kCubeCount * 8;
    for (uint32_t offset = 0; offset < kPositionCount; offset += 4) {
        const auto position_x =
            vld1q_f32(cube_group.pos[0].data() + offset);
        const auto position_y =
            vld1q_f32(cube_group.pos[1].data() + offset);
        const auto position_z =
            vld1q_f32(cube_group.pos[2].data() + offset);
        const auto position = internal::TransformPointNeon(
            state.pose, position_x, position_y, position_z);
        vst1q_f32(out.pos[0].data() + offset, position.x);
        vst1q_f32(out.pos[1].data() + offset, position.y);
        vst1q_f32(out.pos[2].data() + offset, position.z);
    }

    constexpr uint32_t kQuadAttrCount =
        kCubeCount == 1 ? 8U : CubeGroupType::kQuadAttrCapacity;
    for (uint32_t offset = 0; offset < kQuadAttrCount; offset += 4) {
        const auto source_normal_x =
            vld1q_f32(cube_group.normal[0].data() + offset);
        const auto source_normal_y =
            vld1q_f32(cube_group.normal[1].data() + offset);
        const auto source_normal_z =
            vld1q_f32(cube_group.normal[2].data() + offset);
        const auto plane_d =
            vld1q_f32(cube_group.plane_d.data() + offset);
        auto facing = internal::DotNeon(
            source_normal_x, source_normal_y, source_normal_z, plane_d,
            state.facing_coeff);
        facing = vmulq_f32(
            facing, vld1q_f32(cube_group.winding_sign.data() + offset));
        const auto back_face =
            vcleq_f32(facing, vdupq_n_f32(0.0f));
        internal::StoreBackFacesNeon(out.back_face.data() + offset,
                                     back_face);

        auto normal = internal::TransformDirectionNeon(
            state.normal, source_normal_x, source_normal_y,
            source_normal_z);

        if constexpr (kIris && kHasPbr && !kPosOnly) {
            const auto source_tangent_x =
                vld1q_f32(cube_group.tangent[0].data() + offset);
            const auto source_tangent_y =
                vld1q_f32(cube_group.tangent[1].data() + offset);
            const auto source_tangent_z =
                vld1q_f32(cube_group.tangent[2].data() + offset);
            auto tangent =
                state.uniform_scale
                    ? internal::TransformDirectionNeon(
                          state.normal, source_tangent_x, source_tangent_y,
                          source_tangent_z)
                    : internal::TransformDirectionNeon(
                          state.pose, source_tangent_x, source_tangent_y,
                          source_tangent_z);
            if (!state.uniform_scale) [[unlikely]] {
                internal::NormalizeDirectionsNeon(normal, tangent);
            }

            const auto reverse_sign =
                internal::BackFaceSignNeon(back_face);
            normal.x = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(normal.x),
                vreinterpretq_u32_f32(reverse_sign)));
            normal.y = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(normal.y),
                vreinterpretq_u32_f32(reverse_sign)));
            normal.z = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(normal.z),
                vreinterpretq_u32_f32(reverse_sign)));
            vst1q_u32(out.normal.data() + offset,
                      internal::PackSnorm3x8Neon(
                          normal.x, normal.y, normal.z));

            const auto baked_handedness =
                vld1q_f32(cube_group.tangent[3].data() + offset);
            auto tangent_w = vmulq_n_f32(
                baked_handedness, state.tangent_orientation);
            tangent_w = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(tangent_w),
                vreinterpretq_u32_f32(reverse_sign)));
            const auto degenerate = vceqq_f32(
                baked_handedness, vdupq_n_f32(0.0f));
            tangent_w = vbslq_f32(degenerate, vdupq_n_f32(1.0f),
                                  tangent_w);
            vst1q_u32(out.tangent.data() + offset,
                      internal::PackSnorm4x8Neon(
                          tangent.x, tangent.y, tangent.z, tangent_w));
        } else {
            if (!state.uniform_scale) [[unlikely]] {
                internal::NormalizeDirectionNeon(normal);
            }

            const auto reverse_sign =
                internal::BackFaceSignNeon(back_face);
            normal.x = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(normal.x),
                vreinterpretq_u32_f32(reverse_sign)));
            normal.y = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(normal.y),
                vreinterpretq_u32_f32(reverse_sign)));
            normal.z = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(normal.z),
                vreinterpretq_u32_f32(reverse_sign)));
            vst1q_u32(out.normal.data() + offset,
                      internal::PackSnorm3x8Neon(
                          normal.x, normal.y, normal.z));
        }

        if constexpr (CubeGroupType::kTranslucent) {
            const auto center_x =
                vld1q_f32(cube_group.center[0].data() + offset);
            const auto center_y =
                vld1q_f32(cube_group.center[1].data() + offset);
            const auto center_z =
                vld1q_f32(cube_group.center[2].data() + offset);
            const auto center_w = vdupq_n_f32(1.0f);
            const auto clip_z = internal::DotNeon(
                center_x, center_y, center_z, center_w, state.depth_z);
            const auto clip_w = internal::DotNeon(
                center_x, center_y, center_z, center_w, state.depth_w);
            vst1q_f32(out.face_depth.data() + offset,
                      vdivq_f32(clip_z, clip_w));
        }
    }

    static_cast<void>(kCulling);
}

}  // namespace internal

template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
    requires(CubeGroupType::kCubeGroupCapacity == 2)
YSM_INLINE void Transform(simd::Tag<simd::Type::NEON>,
                          const CubeGroupType& cube_group,
                          const RenderBoneState& state,
                          CubeOutput<CubeGroupType>& out) {
    if (cube_group.cube_count == 1) {
        if (cube_group.cube_attr[0].quad_count == 1) {
            internal::TransformSingleQuadNeon<
                kCulling, kIris, kHasPbr, kPosOnly>(cube_group, state, out);
        } else {
            internal::TransformCubesNeon<
                1, kCulling, kIris, kHasPbr, kPosOnly>(
                cube_group, state, out);
        }
        return;
    }

    internal::TransformCubesNeon<
        2, kCulling, kIris, kHasPbr, kPosOnly>(cube_group, state, out);
}
}  // namespace ysm::gfx::renderer::cube

YSM_FAST_MATH_END
