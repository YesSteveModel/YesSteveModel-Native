#pragma once

#include <immintrin.h>
#include <limits>

#include "gfx/bake/baked_model.h"
#include "renderer/render_state.h"
#include "cpu.h"
#include "fast_math.h"
#include "inline.h"
#include "compiler.h"
#include "output.h"
#include "transform_sse41.h"

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::renderer::cube {

namespace internal {

struct Avx2Vec3 {
    __m256 x;
    __m256 y;
    __m256 z;
};

YSM_TARGET_AVX2 YSM_INLINE __m256 InverseSqrtAvx2(__m256 value) noexcept {
#if YSM_TRANSFORM_USE_APPROXIMATE_RSQRT
    return _mm256_rsqrt_ps(value);
#else
    return _mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_sqrt_ps(value));
#endif
}

YSM_TARGET_AVX2 YSM_INLINE Avx2Vec3
TransformPointAvx2(const mat4 matrix, __m256 x, __m256 y, __m256 z) noexcept {
    const auto x_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][0]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][0])));
    const auto x_zt = _mm256_fmadd_ps(z, _mm256_set1_ps(matrix[2][0]),
                                      _mm256_set1_ps(matrix[3][0]));
    const auto y_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][1]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][1])));
    const auto y_zt = _mm256_fmadd_ps(z, _mm256_set1_ps(matrix[2][1]),
                                      _mm256_set1_ps(matrix[3][1]));
    const auto z_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][2]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][2])));
    const auto z_zt = _mm256_fmadd_ps(z, _mm256_set1_ps(matrix[2][2]),
                                      _mm256_set1_ps(matrix[3][2]));
    return {_mm256_add_ps(x_xy, x_zt), _mm256_add_ps(y_xy, y_zt),
            _mm256_add_ps(z_xy, z_zt)};
}

YSM_TARGET_AVX2 YSM_INLINE Avx2Vec3 TransformDirectionAvx2(
    const mat3 matrix, __m256 x, __m256 y, __m256 z) noexcept {
    const auto x_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][0]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][0])));
    const auto x_z = _mm256_mul_ps(z, _mm256_set1_ps(matrix[2][0]));
    const auto y_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][1]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][1])));
    const auto y_z = _mm256_mul_ps(z, _mm256_set1_ps(matrix[2][1]));
    const auto z_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][2]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][2])));
    const auto z_z = _mm256_mul_ps(z, _mm256_set1_ps(matrix[2][2]));
    return {_mm256_add_ps(x_xy, x_z), _mm256_add_ps(y_xy, y_z),
            _mm256_add_ps(z_xy, z_z)};
}

YSM_TARGET_AVX2 YSM_INLINE Avx2Vec3 TransformDirectionAvx2(
    const mat4 matrix, __m256 x, __m256 y, __m256 z) noexcept {
    const auto x_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][0]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][0])));
    const auto x_z = _mm256_mul_ps(z, _mm256_set1_ps(matrix[2][0]));
    const auto y_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][1]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][1])));
    const auto y_z = _mm256_mul_ps(z, _mm256_set1_ps(matrix[2][1]));
    const auto z_xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(matrix[1][2]),
                        _mm256_mul_ps(x, _mm256_set1_ps(matrix[0][2])));
    const auto z_z = _mm256_mul_ps(z, _mm256_set1_ps(matrix[2][2]));
    return {_mm256_add_ps(x_xy, x_z), _mm256_add_ps(y_xy, y_z),
            _mm256_add_ps(z_xy, z_z)};
}

YSM_TARGET_AVX2 YSM_INLINE __m256 DotAvx2(
    __m256 x, __m256 y, __m256 z, __m256 w,
    const vec4 coefficients) noexcept {
    const auto xy =
        _mm256_fmadd_ps(y, _mm256_set1_ps(coefficients[1]),
                        _mm256_mul_ps(x, _mm256_set1_ps(coefficients[0])));
    const auto zw =
        _mm256_fmadd_ps(w, _mm256_set1_ps(coefficients[3]),
                        _mm256_mul_ps(z, _mm256_set1_ps(coefficients[2])));
    return _mm256_add_ps(xy, zw);
}

YSM_TARGET_AVX2 YSM_INLINE void NormalizeDirectionAvx2(
    Avx2Vec3& direction) noexcept {
    const auto length_xy = _mm256_fmadd_ps(
        direction.y, direction.y, _mm256_mul_ps(direction.x, direction.x));
    const auto length_z = _mm256_mul_ps(direction.z, direction.z);
    const auto length_squared = _mm256_add_ps(length_xy, length_z);
    const auto valid = _mm256_and_ps(
        _mm256_cmp_ps(length_squared, _mm256_setzero_ps(), _CMP_GT_OQ),
        _mm256_cmp_ps(length_squared,
                      _mm256_set1_ps(std::numeric_limits<float>::max()),
                      _CMP_LE_OQ));
    const auto one = _mm256_set1_ps(1.0f);
    const auto safe_length_squared =
        _mm256_blendv_ps(one, length_squared, valid);
    const auto inverse_length = InverseSqrtAvx2(safe_length_squared);
    direction.x =
        _mm256_and_ps(valid, _mm256_mul_ps(direction.x, inverse_length));
    direction.y =
        _mm256_and_ps(valid, _mm256_mul_ps(direction.y, inverse_length));
    direction.z =
        _mm256_and_ps(valid, _mm256_mul_ps(direction.z, inverse_length));
}

YSM_TARGET_AVX2 YSM_INLINE void NormalizeDirectionsAvx2(
    Avx2Vec3& first, Avx2Vec3& second) noexcept {
    const auto first_xy =
        _mm256_fmadd_ps(first.y, first.y, _mm256_mul_ps(first.x, first.x));
    const auto second_xy =
        _mm256_fmadd_ps(second.y, second.y, _mm256_mul_ps(second.x, second.x));
    const auto first_z = _mm256_mul_ps(first.z, first.z);
    const auto second_z = _mm256_mul_ps(second.z, second.z);
    const auto first_length_squared = _mm256_add_ps(first_xy, first_z);
    const auto second_length_squared = _mm256_add_ps(second_xy, second_z);
    const auto zero = _mm256_setzero_ps();
    const auto finite_limit =
        _mm256_set1_ps(std::numeric_limits<float>::max());
    const auto first_valid = _mm256_and_ps(
        _mm256_cmp_ps(first_length_squared, zero, _CMP_GT_OQ),
        _mm256_cmp_ps(first_length_squared, finite_limit, _CMP_LE_OQ));
    const auto second_valid = _mm256_and_ps(
        _mm256_cmp_ps(second_length_squared, zero, _CMP_GT_OQ),
        _mm256_cmp_ps(second_length_squared, finite_limit, _CMP_LE_OQ));
    const auto one = _mm256_set1_ps(1.0f);
    const auto first_inverse = InverseSqrtAvx2(
        _mm256_blendv_ps(one, first_length_squared, first_valid));
    const auto second_inverse = InverseSqrtAvx2(
        _mm256_blendv_ps(one, second_length_squared, second_valid));
    first.x =
        _mm256_and_ps(first_valid, _mm256_mul_ps(first.x, first_inverse));
    second.x =
        _mm256_and_ps(second_valid, _mm256_mul_ps(second.x, second_inverse));
    first.y =
        _mm256_and_ps(first_valid, _mm256_mul_ps(first.y, first_inverse));
    second.y =
        _mm256_and_ps(second_valid, _mm256_mul_ps(second.y, second_inverse));
    first.z =
        _mm256_and_ps(first_valid, _mm256_mul_ps(first.z, first_inverse));
    second.z =
        _mm256_and_ps(second_valid, _mm256_mul_ps(second.z, second_inverse));
}

YSM_TARGET_AVX2 YSM_INLINE __m256i ConvertSnorm8Avx2(
    __m256 value) noexcept {
    const auto scaled = _mm256_mul_ps(value, _mm256_set1_ps(127.0f));
    const auto rounded = _mm256_round_ps(
        scaled, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    auto result = _mm256_cvtps_epi32(rounded);
    result = _mm256_max_epi32(
        _mm256_set1_epi32(-127),
        _mm256_min_epi32(_mm256_set1_epi32(127), result));
    return _mm256_and_si256(result, _mm256_set1_epi32(0xff));
}

YSM_TARGET_AVX2 YSM_INLINE __m256i PackSnorm4x8Avx2(
    __m256 x, __m256 y, __m256 z, __m256 w) noexcept {
    const auto packed_x = ConvertSnorm8Avx2(x);
    const auto packed_y = _mm256_slli_epi32(ConvertSnorm8Avx2(y), 8);
    const auto packed_xy = _mm256_or_si256(packed_x, packed_y);
    const auto packed_z = _mm256_slli_epi32(ConvertSnorm8Avx2(z), 16);
    const auto packed_w = _mm256_slli_epi32(ConvertSnorm8Avx2(w), 24);
    const auto packed_zw = _mm256_or_si256(packed_z, packed_w);
    return _mm256_or_si256(packed_xy, packed_zw);
}

YSM_TARGET_AVX2 YSM_INLINE uint32_t StoreBackFacesAvx2(
    std::array<bool, 8>& destination, __m256 back_face) noexcept {
    const auto values = _mm256_and_si256(_mm256_castps_si256(back_face),
                                         _mm256_set1_epi32(1));
    const auto packed16 =
        _mm_packus_epi32(_mm256_castsi256_si128(values),
                         _mm256_extracti128_si256(values, 1));
    const auto packed8 = _mm_packus_epi16(packed16, packed16);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(destination.data()), packed8);
    auto back_face_bits =
        static_cast<uint32_t>(_mm256_movemask_ps(back_face));
#ifndef YSM_MSVC_FRONTEND
    asm volatile("" : "+r"(back_face_bits));    // 防溢出
#endif
    return back_face_bits;
}

YSM_TARGET_AVX2 YSM_INLINE __m256 BackFaceSignAvx2(
    uint32_t back_face_bits) noexcept {
    const auto bits = _mm256_set1_epi32(static_cast<int32_t>(back_face_bits));
    const auto shifts = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    return _mm256_castsi256_ps(
        _mm256_slli_epi32(_mm256_srlv_epi32(bits, shifts), 31));
}

}  // namespace internal

template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
    requires(CubeGroupType::kCubeGroupCapacity == 1)
YSM_TARGET_AVX2 void Transform(simd::Tag<simd::Type::AVX2>,
                               const CubeGroupType& cube_group,
                               const RenderBoneState& state,
                               CubeOutput<CubeGroupType>& out) {
    if (cube_group.IsSingleQuad()) {
        internal::TransformSingleQuadSse41<
            kCulling, kIris, kHasPbr, kPosOnly>(cube_group, state, out);
        return;
    }

    const auto position_x = _mm256_load_ps(cube_group.pos[0].data());
    const auto position_y = _mm256_load_ps(cube_group.pos[1].data());
    const auto position_z = _mm256_load_ps(cube_group.pos[2].data());
    const auto position = internal::TransformPointAvx2(
        state.pose, position_x, position_y, position_z);
    _mm256_store_ps(out.pos[0].data(), position.x);
    _mm256_store_ps(out.pos[1].data(), position.y);
    _mm256_store_ps(out.pos[2].data(), position.z);

    const auto source_normal_x = _mm256_load_ps(cube_group.normal[0].data());
    const auto source_normal_y = _mm256_load_ps(cube_group.normal[1].data());
    const auto source_normal_z = _mm256_load_ps(cube_group.normal[2].data());
    const auto plane_d = _mm256_load_ps(cube_group.plane_d.data());
    auto facing =
        internal::DotAvx2(source_normal_x, source_normal_y, source_normal_z,
                          plane_d, state.facing_coeff);
    facing =
        _mm256_mul_ps(facing, _mm256_load_ps(cube_group.winding_sign.data()));
    const auto back_face =
        _mm256_cmp_ps(facing, _mm256_setzero_ps(), _CMP_LE_OQ);
    const auto back_face_bits =
        internal::StoreBackFacesAvx2(out.back_face, back_face);

    auto normal = internal::TransformDirectionAvx2(
        state.normal, source_normal_x, source_normal_y, source_normal_z);

    if constexpr (kIris && kHasPbr && !kPosOnly) {
        const auto source_tangent_x =
            _mm256_load_ps(cube_group.tangent[0].data());
        const auto source_tangent_y =
            _mm256_load_ps(cube_group.tangent[1].data());
        const auto source_tangent_z =
            _mm256_load_ps(cube_group.tangent[2].data());
        auto tangent = state.uniform_scale
                           ? internal::TransformDirectionAvx2(
                                 state.normal, source_tangent_x,
                                 source_tangent_y, source_tangent_z)
                           : internal::TransformDirectionAvx2(
                                 state.pose, source_tangent_x, source_tangent_y,
                                 source_tangent_z);
        if (!state.uniform_scale) [[unlikely]] {
            internal::NormalizeDirectionsAvx2(normal, tangent);
        }

        const auto reverse_sign =
            internal::BackFaceSignAvx2(back_face_bits);
        normal.x = _mm256_xor_ps(normal.x, reverse_sign);
        normal.y = _mm256_xor_ps(normal.y, reverse_sign);
        normal.z = _mm256_xor_ps(normal.z, reverse_sign);
        const auto zero = _mm256_setzero_ps();
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(out.normal.data()),
            internal::PackSnorm4x8Avx2(normal.x, normal.y, normal.z, zero));

        const auto baked_handedness =
            _mm256_load_ps(cube_group.tangent[3].data());
        auto tangent_w = _mm256_mul_ps(
            baked_handedness, _mm256_set1_ps(state.tangent_orientation));
        tangent_w = _mm256_xor_ps(tangent_w, reverse_sign);
        const auto degenerate =
            _mm256_cmp_ps(baked_handedness, zero, _CMP_EQ_OQ);
        tangent_w = _mm256_blendv_ps(tangent_w, _mm256_set1_ps(1.0f),
                                     degenerate);
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(out.tangent.data()),
            internal::PackSnorm4x8Avx2(tangent.x, tangent.y, tangent.z,
                                       tangent_w));
    } else {
        if (!state.uniform_scale) [[unlikely]] {
            internal::NormalizeDirectionAvx2(normal);
        }

        const auto reverse_sign =
            internal::BackFaceSignAvx2(back_face_bits);
        normal.x = _mm256_xor_ps(normal.x, reverse_sign);
        normal.y = _mm256_xor_ps(normal.y, reverse_sign);
        normal.z = _mm256_xor_ps(normal.z, reverse_sign);
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(out.normal.data()),
            internal::PackSnorm4x8Avx2(normal.x, normal.y, normal.z,
                                       _mm256_setzero_ps()));
    }

    if constexpr (CubeGroupType::kTranslucent) {
        const auto center_x = _mm256_load_ps(cube_group.center[0].data());
        const auto center_y = _mm256_load_ps(cube_group.center[1].data());
        const auto center_z = _mm256_load_ps(cube_group.center[2].data());
        const auto center_w = _mm256_set1_ps(1.0f);
        const auto clip_z = internal::DotAvx2(center_x, center_y, center_z,
                                              center_w, state.depth_z);
        const auto clip_w = internal::DotAvx2(center_x, center_y, center_z,
                                              center_w, state.depth_w);
        _mm256_store_ps(out.face_depth.data(), _mm256_div_ps(clip_z, clip_w));
    }

    static_cast<void>(kCulling);
}
}  // namespace ysm::gfx::renderer::cube

YSM_FAST_MATH_END
