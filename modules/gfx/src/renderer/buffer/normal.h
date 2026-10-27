#pragma once

#include <cglm/types.h>
#include <bit>
#include <array>
#include <cmath>
#include "cpu.h"
#include "inline.h"

#ifdef YSM_X64
#include <immintrin.h>
#elif (defined YSM_ARM64)
#include <arm_neon.h>
#endif

namespace ysm::gfx::renderer::buffer {
namespace internal {
template <bool kNormal>
YSM_INLINE uint32_t PackArray(const std::array<int32_t, 4>& result) {
    auto pack = [](const int32_t v) noexcept -> uint32_t {
        return std::bit_cast<uint8_t>(static_cast<int8_t>(v));
    };

    // little endian
    if constexpr (!kNormal) {
        return pack(result[0])
             | pack(result[1]) << 8
             | pack(result[2]) << 16
             | pack(result[3]) << 24;
    } else {
        return pack(result[0])
             | pack(result[1]) << 8
             | pack(result[2]) << 16;
    }
}
}  // namespace internal

#ifdef YSM_ARM64
template <bool kNormal>
YSM_INLINE uint32_t PackSnorm4x8(simd::Tag<simd::Type::NEON>, vec4 value) noexcept {
    CGLM_ALIGN(16) std::array<int32_t, 4> result;
    auto m = vld1q_f32(value);
    m = vmulq_f32(m, vdupq_n_f32(127.0f));

    auto mi = vcvtnq_s32_f32(m);
    mi = vmaxq_s32(vdupq_n_s32(-127), vminq_s32(vdupq_n_s32(127), mi));

    vst1q_s32(result.data(), mi);
    return internal::PackArray<kNormal>(result);
}
#endif

#ifdef YSM_X64
template <bool kNormal, simd::Type kType> requires (kType != simd::Type::kNone)
YSM_TARGET_SSE41
YSM_INLINE uint32_t PackSnorm4x8(simd::Tag<kType>, vec4 value) noexcept {
    CGLM_ALIGN(16) std::array<int32_t, 4> result;
    auto m = _mm_load_ps(value);
    m = _mm_mul_ps(m, _mm_set1_ps(127.0f));
    m = _mm_round_ps(m, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);

    auto mi = _mm_cvtps_epi32(m);
    mi = _mm_max_epi32(_mm_set1_epi32(-127), _mm_min_epi32(_mm_set1_epi32(127), mi));

    _mm_store_si128(reinterpret_cast<__m128i *>(result.data()), mi);

    return internal::PackArray<kNormal>(result);
}
#endif

template <bool kNormal>
YSM_INLINE uint32_t PackSnorm4x8(simd::GenericTag, vec4 value) noexcept {
    auto pack = [](float v) noexcept -> uint32_t {
        return std::bit_cast<uint8_t>(static_cast<int8_t>(
            std::roundf(std::clamp(v, -1.0f, 1.0f) * 127.0f)
        ));
    };

    if constexpr (!kNormal) {
        return pack(value[0])
             | pack(value[1]) << 8
             | pack(value[2]) << 16
             | pack(value[3]) << 24;
    } else {
        return pack(value[0])
             | pack(value[1]) << 8
             | pack(value[2]) << 16;
    }
}
}
