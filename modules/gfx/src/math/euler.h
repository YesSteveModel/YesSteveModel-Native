#pragma once

#include <cglm/euler.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

#include <cglm/types.h>

#include "fast_math.h"
#include "inline.h"
#include "compiler.h"
#include "cpu.h"

#ifdef YSM_X64
#include <immintrin.h>
#elif defined(YSM_ARM64)
#include <arm_neon.h>
#endif

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::math {
namespace internal {
constexpr float kSimdSinCosLimit = 8192.0f;

YSM_INLINE double ModuloTwoPi(double value) noexcept {
    constexpr double kPi = std::numbers::pi_v<double>;
    constexpr double kTwoPi = 2.0 * kPi;
    constexpr uint64_t kFractionMask = (uint64_t{1} << 52) - 1;
    constexpr uint64_t kImplicitBit = uint64_t{1} << 52;
    constexpr uint64_t kTwoPiBits = std::bit_cast<uint64_t>(kTwoPi);
    constexpr int kTwoPiExponent =
        static_cast<int>((kTwoPiBits >> 52) & 0x7ff);
    constexpr uint64_t kTwoPiSignificand =
        (kTwoPiBits & kFractionMask) | kImplicitBit;

    uint64_t value_bits = std::bit_cast<uint64_t>(value);
    int exponent = static_cast<int>((value_bits >> 52) & 0x7ff);
    if (exponent == 0x7ff) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value_bits <= kTwoPiBits) {
        return value_bits == kTwoPiBits ? 0.0 : value;
    }

    uint64_t significand = (value_bits & kFractionMask) | kImplicitBit;
    for (; exponent > kTwoPiExponent; --exponent) {
        const uint64_t difference = significand - kTwoPiSignificand;
        if ((difference >> 63) == 0) {
            if (difference == 0) {
                return 0.0;
            }
            significand = difference;
        }
        significand <<= 1;
    }

    const uint64_t difference = significand - kTwoPiSignificand;
    if ((difference >> 63) == 0) {
        if (difference == 0) {
            return 0.0;
        }
        significand = difference;
    }
    while ((significand & kImplicitBit) == 0) {
        significand <<= 1;
        --exponent;
    }
    if (exponent > 0) {
        value_bits =
            (significand - kImplicitBit) |
            static_cast<uint64_t>(exponent) << 52;
    } else {
        value_bits = significand >> (-exponent + 1);
    }
    return std::bit_cast<double>(value_bits);
}

YSM_INLINE float ReduceAngle(float angle) noexcept {
    constexpr double kPi = std::numbers::pi_v<double>;
    constexpr double kTwoPi = 2.0 * kPi;
    const bool negative =
        (std::bit_cast<uint32_t>(angle) >> 31) != 0;
    double reduced =
        ModuloTwoPi(std::abs(static_cast<double>(angle)));
    if (negative) {
        reduced = -reduced;
    }
    if (reduced > kPi) {
        reduced -= kTwoPi;
    }
    return static_cast<float>(reduced <= -kPi ? reduced + kTwoPi
                                              : reduced);
}

#ifdef YSM_X64

struct SinCos {
    __m128 sin;
    __m128 cos;
};

YSM_INLINE __m128 LoadEulerAngles(vec3 angles) noexcept {
    float angle_x = angles[0];
    float angle_y = angles[1];
    float angle_z = angles[2];
    __m128 input = _mm_setr_ps(angle_x, angle_y, angle_z, 0.0f);
    const __m128 magnitude =
        _mm_andnot_ps(_mm_set1_ps(-0.0f), input);
    const __m128 over_limit = _mm_cmpgt_ps(
        magnitude, _mm_set1_ps(kSimdSinCosLimit));
    if (_mm_movemask_ps(over_limit) != 0) [[unlikely]] {
        if (std::abs(angle_x) > kSimdSinCosLimit) {
            angle_x = ReduceAngle(angle_x);
        }
        if (std::abs(angle_y) > kSimdSinCosLimit) {
            angle_y = ReduceAngle(angle_y);
        }
        if (std::abs(angle_z) > kSimdSinCosLimit) {
            angle_z = ReduceAngle(angle_z);
        }
        input = _mm_setr_ps(angle_x, angle_y, angle_z, 0.0f);
    }
    return input;
}

YSM_TARGET_AVX2
YSM_INLINE SinCos SinCosApproxFmaAvx2(__m128 input) noexcept {
    alignas(32) static constexpr float kSinMagnitudeTable[8] = {
        0.0f,           0x1.87de2ap-2f, 0x1.6a09e6p-1f,
        0x1.d906bcp-1f, 1.0f,           0x1.d906bcp-1f,
        0x1.6a09e6p-1f, 0x1.87de2ap-2f,
    };

    const __m128 scaled = _mm_mul_ps(input, _mm_set1_ps(0x1.45f306p+1f));
    const __m128i indices = _mm_cvtps_epi32(scaled);
    const __m128 k = _mm_cvtepi32_ps(indices);
    __m128 reduced = _mm_fmadd_ps(k, _mm_set1_ps(-0x1.921f8p-2f), input);
    reduced = _mm_fmadd_ps(k, _mm_set1_ps(-0x1.aa22p-21f), reduced);
    reduced = _mm_fmadd_ps(k, _mm_set1_ps(-0x1.68c234p-41f), reduced);

    const __m128i cos_indices =
        _mm_add_epi32(indices, _mm_set1_epi32(4));
    const __m256i table_indices = _mm256_inserti128_si256(
        _mm256_castsi128_si256(indices), cos_indices, 1);
    const __m256 magnitudes = _mm256_permutevar8x32_ps(
        _mm256_load_ps(kSinMagnitudeTable), table_indices);
    const __m256i sign_bits = _mm256_slli_epi32(
        _mm256_and_si256(table_indices, _mm256_set1_epi32(8)), 28);
    const __m256 table_values = _mm256_xor_ps(
        magnitudes, _mm256_castsi256_ps(sign_bits));
    const __m128 sin_k = _mm256_castps256_ps128(table_values);
    const __m128 cos_k = _mm256_extractf128_ps(table_values, 1);

    const __m128 square = _mm_mul_ps(reduced, reduced);
    const __m128 sin_coefficient =
        _mm_fmadd_ps(square, _mm_set1_ps(0x1.111112p-7f),
            _mm_set1_ps(-0x1.555556p-3f));
    const __m128 cos_coefficient =
        _mm_fmadd_ps(square, _mm_set1_ps(0x1.54b8bep-5f),
            _mm_set1_ps(-0x1.ffffc4p-2f));
    const __m128 sin_reduced =
        _mm_fmadd_ps(_mm_mul_ps(square, reduced), sin_coefficient, reduced);
    const __m128 cos_reduced =
        _mm_fmadd_ps(square, cos_coefficient, _mm_set1_ps(1.0f));
    const __m128 sin =
        _mm_fmadd_ps(cos_k, sin_reduced, _mm_mul_ps(sin_k, cos_reduced));
    const __m128 neg_sin_k =
        _mm_xor_ps(sin_k, _mm_set1_ps(-0.0f));
    const __m128 cos =
        _mm_fmadd_ps(neg_sin_k, sin_reduced, _mm_mul_ps(cos_k, cos_reduced));
    return {sin, cos};
}

YSM_TARGET_AVX512
YSM_INLINE SinCos SinCosApproxFmaAvx512(__m128 input) noexcept {
    alignas(64) static constexpr float kSinTable[16] = {
        0.0f,           0x1.87de2ap-2f,  0x1.6a09e6p-1f,
        0x1.d906bcp-1f, 1.0f,            0x1.d906bcp-1f,
        0x1.6a09e6p-1f, 0x1.87de2ap-2f,  0.0f,
        -0x1.87de2ap-2f, -0x1.6a09e6p-1f, -0x1.d906bcp-1f,
        -1.0f,           -0x1.d906bcp-1f, -0x1.6a09e6p-1f,
        -0x1.87de2ap-2f,
    };

    const __m128 scaled = _mm_mul_ps(input, _mm_set1_ps(0x1.45f306p+1f));
    const __m128i indices = _mm_cvtps_epi32(scaled);
    const __m128 k = _mm_cvtepi32_ps(indices);
    __m128 reduced = _mm_fmadd_ps(k, _mm_set1_ps(-0x1.921f8p-2f), input);
    reduced = _mm_fmadd_ps(k, _mm_set1_ps(-0x1.aa22p-21f), reduced);
    reduced = _mm_fmadd_ps(k, _mm_set1_ps(-0x1.68c234p-41f), reduced);

    const __m128i cos_indices =
        _mm_add_epi32(indices, _mm_set1_epi32(4));
    const __m256i table_indices = _mm256_inserti128_si256(
        _mm256_castsi128_si256(indices), cos_indices, 1);
    __m256 table_low = _mm256_load_ps(kSinTable);
    __m256 table_high = _mm256_load_ps(kSinTable + 8);
#ifndef YSM_MSVC_FRONTEND
    asm volatile("" : "+x"(table_low), "+x"(table_high));
#endif
    const __m256 table_values = _mm256_permutex2var_ps(
        table_low, table_indices, table_high);
    const __m128 sin_k = _mm256_castps256_ps128(table_values);
    const __m128 cos_k = _mm256_extractf128_ps(table_values, 1);

    const __m128 square = _mm_mul_ps(reduced, reduced);
    const __m128 sin_coefficient =
        _mm_fmadd_ps(square, _mm_set1_ps(0x1.111112p-7f),
            _mm_set1_ps(-0x1.555556p-3f));
    const __m128 cos_coefficient =
        _mm_fmadd_ps(square, _mm_set1_ps(0x1.54b8bep-5f),
            _mm_set1_ps(-0x1.ffffc4p-2f));
    const __m128 sin_reduced =
        _mm_fmadd_ps(_mm_mul_ps(square, reduced), sin_coefficient, reduced);
    const __m128 cos_reduced =
        _mm_fmadd_ps(square, cos_coefficient, _mm_set1_ps(1.0f));
    const __m128 sin =
        _mm_fmadd_ps(cos_k, sin_reduced, _mm_mul_ps(sin_k, cos_reduced));
    const __m128 neg_sin_k =
        _mm_xor_ps(sin_k, _mm_set1_ps(-0.0f));
    const __m128 cos =
        _mm_fmadd_ps(neg_sin_k, sin_reduced, _mm_mul_ps(cos_k, cos_reduced));
    return {sin, cos};
}

#elif defined(YSM_ARM64)

struct SinCos {
    float32x4_t sin;
    float32x4_t cos;
};

YSM_INLINE float32x4_t LoadEulerAngles(vec3 angles) noexcept {
    float angle_x = angles[0];
    float angle_y = angles[1];
    float angle_z = angles[2];
    if (std::abs(angle_x) > kSimdSinCosLimit) [[unlikely]] {
        angle_x = ReduceAngle(angle_x);
    }
    if (std::abs(angle_y) > kSimdSinCosLimit) [[unlikely]] {
        angle_y = ReduceAngle(angle_y);
    }
    if (std::abs(angle_z) > kSimdSinCosLimit) [[unlikely]] {
        angle_z = ReduceAngle(angle_z);
    }
    const vec4 values{angle_x, angle_y, angle_z, 0.0f};
    return vld1q_f32(values);
}

YSM_INLINE SinCos SinCosApproxFma(float32x4_t input) noexcept {
    alignas(64) static constexpr float kSinTable[16] = {
        0.0f,           0x1.87de2ap-2f,  0x1.6a09e6p-1f,
        0x1.d906bcp-1f, 1.0f,            0x1.d906bcp-1f,
        0x1.6a09e6p-1f, 0x1.87de2ap-2f,  0.0f,
        -0x1.87de2ap-2f, -0x1.6a09e6p-1f, -0x1.d906bcp-1f,
        -1.0f,           -0x1.d906bcp-1f, -0x1.6a09e6p-1f,
        -0x1.87de2ap-2f,
    };
    alignas(16) static constexpr uint8_t kByteLanes[16] = {
        0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    };

    const float32x4_t scaled =
        vmulq_n_f32(input, 0x1.45f306p+1f);
    const int32x4_t indices = vcvtnq_s32_f32(scaled);
    const float32x4_t k = vcvtq_f32_s32(indices);
    float32x4_t reduced =
        vfmaq_n_f32(input, k, -0x1.921f8p-2f);
    reduced = vfmaq_n_f32(reduced, k, -0x1.aa22p-21f);
    reduced = vfmaq_n_f32(reduced, k, -0x1.68c234p-41f);

    const uint32x4_t table_indices = vandq_u32(
        vreinterpretq_u32_s32(indices), vdupq_n_u32(15));
    const uint32x4_t sin_offsets =
        vmulq_n_u32(table_indices, 0x04040404U);
    const uint32x4_t cos_offsets = vmulq_n_u32(
        vandq_u32(vaddq_u32(table_indices, vdupq_n_u32(4)),
                  vdupq_n_u32(15)),
        0x04040404U);
    const uint8x16_t byte_lanes = vld1q_u8(kByteLanes);
    const uint8x16_t sin_byte_indices = vaddq_u8(
        vreinterpretq_u8_u32(sin_offsets), byte_lanes);
    const uint8x16_t cos_byte_indices = vaddq_u8(
        vreinterpretq_u8_u32(cos_offsets), byte_lanes);
    const uint8x16x4_t table = vld1q_u8_x4(
        reinterpret_cast<const uint8_t*>(kSinTable));
    const float32x4_t sin_k = vreinterpretq_f32_u8(
        vqtbl4q_u8(table, sin_byte_indices));
    const float32x4_t cos_k = vreinterpretq_f32_u8(
        vqtbl4q_u8(table, cos_byte_indices));

    const float32x4_t square = vmulq_f32(reduced, reduced);
    const float32x4_t sin_coefficient = vfmaq_n_f32(
        vdupq_n_f32(-0x1.555556p-3f), square, 0x1.111112p-7f);
    const float32x4_t cos_coefficient = vfmaq_n_f32(
        vdupq_n_f32(-0x1.ffffc4p-2f), square, 0x1.54b8bep-5f);
    const float32x4_t sin_reduced = vfmaq_f32(
        reduced, vmulq_f32(square, reduced), sin_coefficient);
    const float32x4_t cos_reduced = vfmaq_f32(
        vdupq_n_f32(1.0f), square, cos_coefficient);
    const float32x4_t sin = vfmaq_f32(
        vmulq_f32(sin_k, cos_reduced), cos_k, sin_reduced);
    const float32x4_t cos = vfmaq_f32(
        vmulq_f32(cos_k, cos_reduced), vnegq_f32(sin_k),
        sin_reduced);
    return {sin, cos};
}

#endif

YSM_INLINE void WriteEulerZYX(const vec4 sin_values, const vec4 cos_values,
                              mat4 destination) noexcept {
    const float sx = sin_values[0];
    const float sy = sin_values[1];
    const float sz = sin_values[2];
    const float cx = cos_values[0];
    const float cy = cos_values[1];
    const float cz = cos_values[2];
    const float czsx = cz * sx;
    const float cxcz = cx * cz;
    const float sysz = sy * sz;

    destination[0][0] = cy * cz;
    destination[0][1] = cy * sz;
    destination[0][2] = -sy;
    destination[0][3] = 0.0f;
    destination[1][0] = czsx * sy - cx * sz;
    destination[1][1] = cxcz + sx * sysz;
    destination[1][2] = cy * sx;
    destination[1][3] = 0.0f;
    destination[2][0] = cxcz * sy + sx * sz;
    destination[2][1] = -czsx + cx * sysz;
    destination[2][2] = cx * cy;
    destination[2][3] = 0.0f;
    destination[3][0] = 0.0f;
    destination[3][1] = 0.0f;
    destination[3][2] = 0.0f;
    destination[3][3] = 1.0f;
}
}  // namespace internal

#ifdef YSM_X64

YSM_TARGET_AVX2
YSM_INLINE void EulerZYX(simd::Tag<simd::Type::AVX2>, vec3 angles,
                         mat4 destination) noexcept {
    const __m128 input = internal::LoadEulerAngles(angles);
    const auto [sin, cos] = internal::SinCosApproxFmaAvx2(input);
    vec4 sin_values;
    vec4 cos_values;
    _mm_store_ps(sin_values, sin);
    _mm_store_ps(cos_values, cos);
    internal::WriteEulerZYX(sin_values, cos_values, destination);
}

YSM_TARGET_AVX512
YSM_INLINE void EulerZYX(simd::Tag<simd::Type::AVX512>, vec3 angles,
                         mat4 destination) noexcept {
    const __m128 input = internal::LoadEulerAngles(angles);
    const auto [sin, cos] = internal::SinCosApproxFmaAvx512(input);
    vec4 sin_values;
    vec4 cos_values;
    _mm_store_ps(sin_values, sin);
    _mm_store_ps(cos_values, cos);
    internal::WriteEulerZYX(sin_values, cos_values, destination);
}

#elif defined(YSM_ARM64)

YSM_INLINE void EulerZYX(simd::Tag<simd::Type::NEON>, vec3 angles,
                         mat4 destination) noexcept {
    const float32x4_t input = internal::LoadEulerAngles(angles);
    const auto [sin, cos] = internal::SinCosApproxFma(input);
    vec4 sin_values;
    vec4 cos_values;
    vst1q_f32(sin_values, sin);
    vst1q_f32(cos_values, cos);
    internal::WriteEulerZYX(sin_values, cos_values, destination);
}

#endif

YSM_INLINE void EulerZYX(simd::GenericTag, vec3 angles,
                         mat4 destination) noexcept {
    glm_euler_zyx(angles, destination);
}
}  // namespace ysm::gfx::math

YSM_FAST_MATH_END
