#pragma once

#include <immintrin.h>
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

struct Sse41Vec3 {
    __m128 x;
    __m128 y;
    __m128 z;
};

struct Sse41Vec2u {
    __m128i first;
    __m128i second;
};

YSM_INLINE __m128 InverseSqrtSse41(__m128 value) noexcept {
#if YSM_TRANSFORM_USE_APPROXIMATE_RSQRT
    return _mm_rsqrt_ps(value);
#else
    return _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(value));
#endif
}

YSM_TARGET_SSE41
YSM_INLINE Sse41Vec3 TransformPointSse41(const mat4 matrix, __m128 x,
                                            __m128 y, __m128 z) noexcept {
    auto x_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][0]));
    auto x_zt = _mm_mul_ps(z, _mm_set1_ps(matrix[2][0]));
    x_xy = _mm_add_ps(x_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][0])));
    x_zt = _mm_add_ps(x_zt, _mm_set1_ps(matrix[3][0]));

    auto y_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][1]));
    auto y_zt = _mm_mul_ps(z, _mm_set1_ps(matrix[2][1]));
    y_xy = _mm_add_ps(y_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][1])));
    y_zt = _mm_add_ps(y_zt, _mm_set1_ps(matrix[3][1]));

    auto z_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][2]));
    auto z_zt = _mm_mul_ps(z, _mm_set1_ps(matrix[2][2]));
    z_xy = _mm_add_ps(z_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][2])));
    z_zt = _mm_add_ps(z_zt, _mm_set1_ps(matrix[3][2]));
    return {_mm_add_ps(x_xy, x_zt), _mm_add_ps(y_xy, y_zt),
            _mm_add_ps(z_xy, z_zt)};
}

YSM_TARGET_SSE41
YSM_INLINE Sse41Vec3 TransformDirectionSse41(
    const mat3 matrix, __m128 x, __m128 y, __m128 z) noexcept {
    auto x_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][0]));
    const auto x_z = _mm_mul_ps(z, _mm_set1_ps(matrix[2][0]));
    x_xy = _mm_add_ps(x_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][0])));

    auto y_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][1]));
    const auto y_z = _mm_mul_ps(z, _mm_set1_ps(matrix[2][1]));
    y_xy = _mm_add_ps(y_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][1])));

    auto z_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][2]));
    const auto z_z = _mm_mul_ps(z, _mm_set1_ps(matrix[2][2]));
    z_xy = _mm_add_ps(z_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][2])));
    return {_mm_add_ps(x_xy, x_z), _mm_add_ps(y_xy, y_z),
            _mm_add_ps(z_xy, z_z)};
}

YSM_TARGET_SSE41
YSM_INLINE Sse41Vec3 TransformDirectionSse41(
    const mat4 matrix, __m128 x, __m128 y, __m128 z) noexcept {
    auto x_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][0]));
    const auto x_z = _mm_mul_ps(z, _mm_set1_ps(matrix[2][0]));
    x_xy = _mm_add_ps(x_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][0])));

    auto y_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][1]));
    const auto y_z = _mm_mul_ps(z, _mm_set1_ps(matrix[2][1]));
    y_xy = _mm_add_ps(y_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][1])));

    auto z_xy = _mm_mul_ps(x, _mm_set1_ps(matrix[0][2]));
    const auto z_z = _mm_mul_ps(z, _mm_set1_ps(matrix[2][2]));
    z_xy = _mm_add_ps(z_xy, _mm_mul_ps(y, _mm_set1_ps(matrix[1][2])));
    return {_mm_add_ps(x_xy, x_z), _mm_add_ps(y_xy, y_z),
            _mm_add_ps(z_xy, z_z)};
}

YSM_TARGET_SSE41
YSM_INLINE __m128 DotSse41(__m128 x, __m128 y, __m128 z, __m128 w,
                           const vec4 coefficients) noexcept {
    auto xy = _mm_mul_ps(x, _mm_set1_ps(coefficients[0]));
    auto zw = _mm_mul_ps(z, _mm_set1_ps(coefficients[2]));
    xy = _mm_add_ps(xy, _mm_mul_ps(y, _mm_set1_ps(coefficients[1])));
    zw = _mm_add_ps(zw, _mm_mul_ps(w, _mm_set1_ps(coefficients[3])));
    return _mm_add_ps(xy, zw);
}

YSM_TARGET_SSE41
YSM_INLINE __m128 DirectionValiditySse41(__m128 length_squared) noexcept {
    return _mm_and_ps(
        _mm_cmpgt_ps(length_squared, _mm_setzero_ps()),
        _mm_cmple_ps(length_squared,
                     _mm_set1_ps(std::numeric_limits<float>::max())));
}

YSM_TARGET_SSE41
YSM_INLINE void NormalizeDirectionSse41(Sse41Vec3& direction) noexcept {
    const auto length_xy =
        _mm_add_ps(_mm_mul_ps(direction.x, direction.x),
                   _mm_mul_ps(direction.y, direction.y));
    const auto length_z = _mm_mul_ps(direction.z, direction.z);
    const auto length_squared = _mm_add_ps(length_xy, length_z);
    const auto valid = DirectionValiditySse41(length_squared);
    const auto one = _mm_set1_ps(1.0f);
    const auto safe_length_squared =
        _mm_blendv_ps(one, length_squared, valid);
    const auto inverse_length = InverseSqrtSse41(safe_length_squared);
    direction.x = _mm_and_ps(valid, _mm_mul_ps(direction.x, inverse_length));
    direction.y = _mm_and_ps(valid, _mm_mul_ps(direction.y, inverse_length));
    direction.z = _mm_and_ps(valid, _mm_mul_ps(direction.z, inverse_length));
}

YSM_TARGET_SSE41
YSM_INLINE void NormalizeDirectionsSse41(Sse41Vec3& first,
                                         Sse41Vec3& second) noexcept {
    const auto first_xy =
        _mm_add_ps(_mm_mul_ps(first.x, first.x),
                   _mm_mul_ps(first.y, first.y));
    const auto second_xy =
        _mm_add_ps(_mm_mul_ps(second.x, second.x),
                   _mm_mul_ps(second.y, second.y));
    const auto first_z = _mm_mul_ps(first.z, first.z);
    const auto second_z = _mm_mul_ps(second.z, second.z);
    const auto first_length_squared = _mm_add_ps(first_xy, first_z);
    const auto second_length_squared = _mm_add_ps(second_xy, second_z);
    const auto first_valid = DirectionValiditySse41(first_length_squared);
    const auto second_valid = DirectionValiditySse41(second_length_squared);
    const auto one = _mm_set1_ps(1.0f);
    const auto first_inverse = InverseSqrtSse41(
        _mm_blendv_ps(one, first_length_squared, first_valid));
    const auto second_inverse = InverseSqrtSse41(
        _mm_blendv_ps(one, second_length_squared, second_valid));
    first.x = _mm_and_ps(first_valid, _mm_mul_ps(first.x, first_inverse));
    second.x = _mm_and_ps(second_valid, _mm_mul_ps(second.x, second_inverse));
    first.y = _mm_and_ps(first_valid, _mm_mul_ps(first.y, first_inverse));
    second.y = _mm_and_ps(second_valid, _mm_mul_ps(second.y, second_inverse));
    first.z = _mm_and_ps(first_valid, _mm_mul_ps(first.z, first_inverse));
    second.z = _mm_and_ps(second_valid, _mm_mul_ps(second.z, second_inverse));
}

YSM_TARGET_SSE41
YSM_INLINE __m128i ConvertSnorm8Sse41(__m128 value) noexcept {
    const auto scaled = _mm_mul_ps(value, _mm_set1_ps(127.0f));
    const auto rounded =
        _mm_round_ps(scaled, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    auto result = _mm_cvtps_epi32(rounded);
    result = _mm_max_epi32(
        _mm_set1_epi32(-127),
        _mm_min_epi32(_mm_set1_epi32(127), result));
    return _mm_and_si128(result, _mm_set1_epi32(0xff));
}

YSM_TARGET_SSE41
YSM_INLINE Sse41Vec2u ConvertSnorm8PairSse41(__m128 first,
                                               __m128 second) noexcept {
    const auto scale = _mm_set1_ps(127.0f);
    first = _mm_mul_ps(first, scale);
    second = _mm_mul_ps(second, scale);
    first =
        _mm_round_ps(first, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    second =
        _mm_round_ps(second, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    auto first_int = _mm_cvtps_epi32(first);
    auto second_int = _mm_cvtps_epi32(second);
    const auto lower = _mm_set1_epi32(-127);
    first_int = _mm_max_epi32(lower, first_int);
    second_int = _mm_max_epi32(lower, second_int);
    const auto upper = _mm_set1_epi32(127);
    first_int = _mm_min_epi32(upper, first_int);
    second_int = _mm_min_epi32(upper, second_int);
    const auto byte_mask = _mm_set1_epi32(0xff);
    return {_mm_and_si128(first_int, byte_mask),
            _mm_and_si128(second_int, byte_mask)};
}

YSM_TARGET_SSE41
YSM_INLINE __m128i PackSnorm4x8Sse41(__m128 x, __m128 y, __m128 z,
                                     __m128 w) noexcept {
    const auto packed_xy_values = ConvertSnorm8PairSse41(x, y);
    const auto packed_xy = _mm_or_si128(
        packed_xy_values.first,
        _mm_slli_epi32(packed_xy_values.second, 8));
    const auto packed_zw_values = ConvertSnorm8PairSse41(z, w);
    const auto packed_zw = _mm_or_si128(
        _mm_slli_epi32(packed_zw_values.first, 16),
        _mm_slli_epi32(packed_zw_values.second, 24));
    return _mm_or_si128(packed_xy, packed_zw);
}

YSM_TARGET_SSE41
YSM_INLINE __m128i PackSnorm3x8Sse41(__m128 x, __m128 y,
                                     __m128 z) noexcept {
    const auto packed_xy_values = ConvertSnorm8PairSse41(x, y);
    const auto packed_x = packed_xy_values.first;
    const auto packed_y = _mm_slli_epi32(packed_xy_values.second, 8);
    const auto packed_z = _mm_slli_epi32(ConvertSnorm8Sse41(z), 16);
    return _mm_or_si128(_mm_or_si128(packed_x, packed_y), packed_z);
}

YSM_TARGET_SSE41
YSM_INLINE uint32_t StoreBackFacesSse41(bool* destination,
                                        __m128 back_face) noexcept {
    const auto values =
        _mm_and_si128(_mm_castps_si128(back_face), _mm_set1_epi32(1));
    const auto packed16 = _mm_packus_epi32(values, values);
    const auto packed8 = _mm_packus_epi16(packed16, packed16);
    const auto packed_bytes =
        static_cast<uint32_t>(_mm_cvtsi128_si32(packed8));
    std::memcpy(destination, &packed_bytes, sizeof(packed_bytes));
    return static_cast<uint32_t>(_mm_movemask_ps(back_face));
}

YSM_TARGET_SSE41
YSM_INLINE __m128 BackFaceSignSse41(uint32_t back_face_bits) noexcept {
    const auto bits =
        _mm_set1_epi32(static_cast<int32_t>(back_face_bits));
    const auto lane_bits = _mm_setr_epi32(1, 2, 4, 8);
    const auto selected = _mm_and_si128(bits, lane_bits);
    const auto is_zero = _mm_cmpeq_epi32(selected, _mm_setzero_si128());
    return _mm_castsi128_ps(
        _mm_andnot_si128(is_zero, _mm_set1_epi32(INT32_MIN)));
}

template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
YSM_TARGET_SSE41
YSM_INLINE void TransformSingleQuadSse41(
    const CubeGroupType& cube_group, const RenderBoneState& state,
    CubeOutput<CubeGroupType>& out) {
    const auto position_x = _mm_load_ps(cube_group.pos[0].data());
    const auto position_y = _mm_load_ps(cube_group.pos[1].data());
    const auto position_z = _mm_load_ps(cube_group.pos[2].data());
    const auto position = TransformPointSse41(
        state.pose, position_x, position_y, position_z);
    _mm_store_ps(out.pos[0].data(), position.x);
    _mm_store_ps(out.pos[1].data(), position.y);
    _mm_store_ps(out.pos[2].data(), position.z);

    TransformQuadAttributes<kCulling, kIris, kHasPbr, kPosOnly>(
        simd::Tag<simd::Type::SSE41>{}, cube_group, state, out, 0);
}

template <typename CubeGroupType>
YSM_TARGET_SSE41
YSM_INLINE void TransformPositionBatchSse41(
    const CubeGroupType& cube_group, const RenderBoneState& state,
    CubeOutput<CubeGroupType>& out, uint32_t offset) {
    const auto position_x =
        _mm_load_ps(cube_group.pos[0].data() + offset);
    const auto position_y =
        _mm_load_ps(cube_group.pos[1].data() + offset);
    const auto position_z =
        _mm_load_ps(cube_group.pos[2].data() + offset);
    const auto position = TransformPointSse41(
        state.pose, position_x, position_y, position_z);
    _mm_store_ps(out.pos[0].data() + offset, position.x);
    _mm_store_ps(out.pos[1].data() + offset, position.y);
    _mm_store_ps(out.pos[2].data() + offset, position.z);
}

template <bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
YSM_TARGET_SSE41
YSM_INLINE void TransformQuadBatchSse41(
    const CubeGroupType& cube_group, const RenderBoneState& state,
    CubeOutput<CubeGroupType>& out, uint32_t offset) {
    const auto source_normal_x =
        _mm_load_ps(cube_group.normal[0].data() + offset);
    const auto source_normal_y =
        _mm_load_ps(cube_group.normal[1].data() + offset);
    const auto source_normal_z =
        _mm_load_ps(cube_group.normal[2].data() + offset);
    const auto plane_d =
        _mm_load_ps(cube_group.plane_d.data() + offset);
    auto facing = DotSse41(
        source_normal_x, source_normal_y, source_normal_z, plane_d,
        state.facing_coeff);
    facing = _mm_mul_ps(
        facing, _mm_load_ps(cube_group.winding_sign.data() + offset));
    const auto back_face = _mm_cmple_ps(facing, _mm_setzero_ps());
    const auto back_face_bits = StoreBackFacesSse41(
        out.back_face.data() + offset, back_face);

    auto normal = TransformDirectionSse41(
        state.normal, source_normal_x, source_normal_y, source_normal_z);

    if constexpr (kIris && kHasPbr && !kPosOnly) {
        const auto source_tangent_x =
            _mm_load_ps(cube_group.tangent[0].data() + offset);
        const auto source_tangent_y =
            _mm_load_ps(cube_group.tangent[1].data() + offset);
        const auto source_tangent_z =
            _mm_load_ps(cube_group.tangent[2].data() + offset);
        auto tangent =
            state.uniform_scale
                ? TransformDirectionSse41(
                      state.normal, source_tangent_x, source_tangent_y,
                      source_tangent_z)
                : TransformDirectionSse41(
                      state.pose, source_tangent_x, source_tangent_y,
                      source_tangent_z);
        if (!state.uniform_scale) [[unlikely]] {
            NormalizeDirectionsSse41(normal, tangent);
        }

        const auto reverse_sign = BackFaceSignSse41(back_face_bits);
        normal.x = _mm_xor_ps(normal.x, reverse_sign);
        normal.y = _mm_xor_ps(normal.y, reverse_sign);
        normal.z = _mm_xor_ps(normal.z, reverse_sign);
        const auto zero = _mm_setzero_ps();
        _mm_store_si128(
            reinterpret_cast<__m128i*>(out.normal.data() + offset),
            PackSnorm3x8Sse41(normal.x, normal.y, normal.z));

        const auto baked_handedness =
            _mm_load_ps(cube_group.tangent[3].data() + offset);
        auto tangent_w = _mm_mul_ps(
            baked_handedness, _mm_set1_ps(state.tangent_orientation));
        tangent_w = _mm_xor_ps(tangent_w, reverse_sign);
        const auto degenerate = _mm_cmpeq_ps(baked_handedness, zero);
        tangent_w = _mm_blendv_ps(tangent_w, _mm_set1_ps(1.0f),
                                  degenerate);
        _mm_store_si128(
            reinterpret_cast<__m128i*>(out.tangent.data() + offset),
            PackSnorm4x8Sse41(
                tangent.x, tangent.y, tangent.z, tangent_w));
    } else {
        if (!state.uniform_scale) [[unlikely]] {
            NormalizeDirectionSse41(normal);
        }

        const auto reverse_sign = BackFaceSignSse41(back_face_bits);
        normal.x = _mm_xor_ps(normal.x, reverse_sign);
        normal.y = _mm_xor_ps(normal.y, reverse_sign);
        normal.z = _mm_xor_ps(normal.z, reverse_sign);
        _mm_store_si128(
            reinterpret_cast<__m128i*>(out.normal.data() + offset),
            PackSnorm3x8Sse41(normal.x, normal.y, normal.z));
    }

    if constexpr (CubeGroupType::kTranslucent) {
        const auto center_x =
            _mm_load_ps(cube_group.center[0].data() + offset);
        const auto center_y =
            _mm_load_ps(cube_group.center[1].data() + offset);
        const auto center_z =
            _mm_load_ps(cube_group.center[2].data() + offset);
        const auto center_w = _mm_set1_ps(1.0f);
        const auto clip_z = DotSse41(
            center_x, center_y, center_z, center_w, state.depth_z);
        const auto clip_w = DotSse41(
            center_x, center_y, center_z, center_w, state.depth_w);
        _mm_store_ps(out.face_depth.data() + offset,
                     _mm_div_ps(clip_z, clip_w));
    }
}

template <bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
YSM_TARGET_SSE41
void TransformSingleCubeSse41(const CubeGroupType& cube_group,
                              const RenderBoneState& state,
                              CubeOutput<CubeGroupType>& out) {
    for (uint32_t offset = 0; offset < 8; offset += 4) {
        TransformPositionBatchSse41(cube_group, state, out, offset);
    }
    for (uint32_t offset = 0; offset < 8; offset += 4) {
        TransformQuadBatchSse41<kIris, kHasPbr, kPosOnly>(
            cube_group, state, out, offset);
    }
}

}  // namespace internal

template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
    requires(CubeGroupType::kCubeGroupCapacity == 2)
YSM_TARGET_SSE41
void Transform(simd::Tag<simd::Type::SSE41>,
               const CubeGroupType& cube_group,
               const RenderBoneState& state,
               CubeOutput<CubeGroupType>& out) {
    if (cube_group.cube_count == 1) {
        if (cube_group.cube_attr[0].quad_count == 1) {
            internal::TransformSingleQuadSse41<
                kCulling, kIris, kHasPbr, kPosOnly>(cube_group, state, out);
        } else {
            internal::TransformSingleCubeSse41<
                kIris, kHasPbr, kPosOnly>(cube_group, state, out);
        }
        return;
    }

    for (uint32_t offset = 0; offset < 16; offset += 4) {
        internal::TransformPositionBatchSse41(
            cube_group, state, out, offset);
    }
    for (uint32_t offset = 0; offset < CubeGroupType::kQuadAttrCapacity;
         offset += 4) {
        internal::TransformQuadBatchSse41<
            kIris, kHasPbr, kPosOnly>(cube_group, state, out, offset);
    }

    static_cast<void>(kCulling);
}
}  // namespace ysm::gfx::renderer::cube

YSM_FAST_MATH_END
