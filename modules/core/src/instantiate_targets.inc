#ifndef GUARD_INSTANTIATE_SIMD_TARGETS
#define GUARD_INSTANTIATE_SIMD_TARGETS

#include "cpu.h"

#endif

#ifndef YSM_INSTANTIATE_FILE
#error "YSM_INSTANTIATE_FILE not set"
#endif

#ifdef YSM_X64

#define YSM_SIMD_TYPE ::ysm::simd::Type::AVX512
#define YSM_SIMD_TAG ::ysm::simd::Tag<YSM_SIMD_TYPE>
#define YSM_SIMD_TARGET YSM_TARGET_AVX512
#include YSM_INSTANTIATE_FILE
#undef YSM_SIMD_TARGET
#undef YSM_SIMD_TAG
#undef YSM_SIMD_TYPE

#define YSM_SIMD_TYPE ::ysm::simd::Type::AVX2
#define YSM_SIMD_TAG ::ysm::simd::Tag<YSM_SIMD_TYPE>
#define YSM_SIMD_TARGET YSM_TARGET_AVX2
#include YSM_INSTANTIATE_FILE
#undef YSM_SIMD_TARGET
#undef YSM_SIMD_TAG
#undef YSM_SIMD_TYPE

#define YSM_SIMD_TYPE ::ysm::simd::Type::SSE41
#define YSM_SIMD_TAG ::ysm::simd::Tag<YSM_SIMD_TYPE>
#define YSM_SIMD_TARGET YSM_TARGET_SSE41
#include YSM_INSTANTIATE_FILE
#undef YSM_SIMD_TARGET
#undef YSM_SIMD_TAG
#undef YSM_SIMD_TYPE

#elif YSM_ARM64

#define YSM_SIMD_TYPE ::ysm::simd::Type::NEON
#define YSM_SIMD_TAG ::ysm::simd::Tag<YSM_SIMD_TYPE>
#define YSM_SIMD_TARGET
#include YSM_INSTANTIATE_FILE
#undef YSM_SIMD_TARGET
#undef YSM_SIMD_TAG
#undef YSM_SIMD_TYPE

#endif /* YSM_X64 */

#define YSM_SIMD_TYPE ::ysm::simd::Type::kNone
#define YSM_SIMD_TAG ::ysm::simd::Tag<YSM_SIMD_TYPE>
#define YSM_SIMD_TARGET
#include YSM_INSTANTIATE_FILE
#undef YSM_SIMD_TARGET
#undef YSM_SIMD_TAG
#undef YSM_SIMD_TYPE