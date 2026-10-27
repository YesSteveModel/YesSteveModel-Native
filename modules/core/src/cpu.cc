#include "cpu.h"

#include <string_view>

#include "log.h"
#include "macro.h"

namespace ysm {
#ifdef YSM_X64
cpu_features::X86Features kX86Features;
#elif defined(YSM_ARM64)
cpu_features::Aarch64Features kArm64Feature;
#endif

simd::Type simd::kSupported;

void InitCpuInfo() {
#ifdef YSM_X64
    auto info = cpu_features::GetX86Info();
    kX86Features = info.features;
    // 暂不考虑 nova lake
    if (YSM_SV_LITERAL(CPU_FEATURES_VENDOR_GENUINE_INTEL) != std::string_view(info.vendor) &&
            kX86Features.avx512f && kX86Features.avx512bw && kX86Features.avx512vl) {
        simd::kSupported = simd::Type::AVX512;
    } else if (kX86Features.avx2 && kX86Features.fma3) {
        simd::kSupported = simd::Type::AVX2;
    } else if (kX86Features.sse3 && kX86Features.ssse3 && kX86Features.sse4_1 && kX86Features.popcnt) {
        simd::kSupported = simd::Type::SSE41;
    } else {
        simd::kSupported = simd::Type::kNone;
    }
#elif defined(YSM_ARM64)
    kArm64Feature = cpu_features::GetAarch64Info().features;
    simd::kSupported = simd::Type::NEON;
#else
    simd::kSupported = simd::Type::kNone;
#endif

    YSM_LOG(INFO, "Simd type: {}",
        magic_enum::enum_name(simd::kSupported));
}

void simd::Mock(Type type) {
    kSupported = type;
}
}
