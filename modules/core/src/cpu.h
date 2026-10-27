#pragma once

#include <cstdint>
#include <magic_enum_switch.hpp>
#include <thread>

#ifdef YSM_X64

#include <cpu_features/cpuinfo_x86.h>
#include <emmintrin.h>

#define ysm_pause _mm_pause()
#define ysm_lfence _mm_lfence()
#define ysm_sfence _mm_sfence()
#define ysm_mfence _mm_mfence()

#if defined(__clang__) || defined(__GNUC__)
#define YSM_TARGET_AVX512 __attribute__((target("avx512f,avx512vl,avx512bw")))
#define YSM_TARGET_AVX2 __attribute__((target("avx2,fma")))
#define YSM_TARGET_SSE41 __attribute__((target("sse4.1,sse3,ssse3,popcnt")))
#else
#define YSM_TARGET_AVX512
#define YSM_TARGET_AVX2
#endif

#elif defined(YSM_ARM64)

#include <cpu_features/cpuinfo_aarch64.h>
#include <arm_acle.h>

#define ysm_pause std::this_thread::yield();    // 不要用 __yield
#define ysm_lfence __dmb(0xB)
#define ysm_sfence __dmb(0xA)
#define ysm_mfence __dmb(0xF)

#if defined(__clang__)
#define YSM_TARGET_SVE __attribute__((target("sve")))
#elif defined(__GNUC__)
#define YSM_TARGET_SVE __attribute__((target("+sve")))
#else
#define YSM_TARGET_SVE
#endif

#else
#error "Unsupported cpu arch"
#endif

namespace ysm {
#ifdef YSM_X64
extern cpu_features::X86Features kX86Features;
#elif (defined YSM_ARM64)
extern cpu_features::Aarch64Features kArm64Feature;
#endif

void InitCpuInfo();

namespace simd {
enum class Width : uint8_t {
    B128,
    B256,
    B512,
};

enum class Type : uint16_t {
    kNone,
#ifdef YSM_X64
    SSE41,
    AVX2,
    AVX512,
#elif defined(YSM_ARM64)
    NEON,
   // SVE128,
#endif
};

namespace internal {
template <Type kType>
struct SimdTypeTrait {
    static_assert(static_cast<int>(kType) == -1, "Unsupported simd type or width");
};

template <>
struct SimdTypeTrait<Type::kNone> {
    static constexpr Width kWidth = Width::B256;
};

#ifdef YSM_X64

template <>
struct SimdTypeTrait<Type::SSE41> {
    static constexpr Width kWidth = Width::B128;
};

template <>
struct SimdTypeTrait<Type::AVX2> {
    static constexpr Width kWidth = Width::B256;
};

template <>
struct SimdTypeTrait<Type::AVX512> {
    static constexpr Width kWidth = Width::B512;
};

#elif defined(YSM_ARM64)

template <>
struct SimdTypeTrait<Type::NEON> {
    static constexpr Width kWidth = Width::B128;
};

#endif
} // internal

template <Type T>
struct Tag {
    constexpr static Type kType = T;

    consteval operator Tag<Type::kNone>() const noexcept {
        return Tag<Type::kNone>{};
    }
};

using GenericTag = Tag<Type::kNone>;

extern Type kSupported;

void Mock(Type type);

constexpr size_t bytes(Width width) noexcept {
    return 16ULL << static_cast<size_t>(width);
}

constexpr size_t ints(Width width) noexcept {
    return bytes(width) / 4;
}

constexpr size_t floats(Width width) noexcept {
    return bytes(width) / 4;
}

template <Type kType>
constexpr Width width(Tag<kType> = {}) noexcept {
    return internal::SimdTypeTrait<kType>::kWidth;
}

template <Type kType>
constexpr size_t bytes(Tag<kType> = {}) noexcept {
    return bytes(width<kType>());
}

template <Type kType>
constexpr size_t ints(Tag<kType> = {}) noexcept {
    return ints(width<kType>());
}

template <Type kType>
constexpr size_t floats(Tag<kType> = {}) noexcept {
    return floats(width<kType>());
}
}
}
