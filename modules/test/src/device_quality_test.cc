#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "cpu.h"
#include "device_quality.h"

#if YSM_WINDOWS
#include <windows.h>
#endif

namespace ysm {
namespace {
constexpr int64_t kGiB = int64_t{1} << 30;

DeviceFacts HighDevice() {
    return {DevicePlatform::kDesktop, DeviceArchitecture::kX86,
            16 * kGiB, 6, true};
}

TEST(DeviceQualityTest, MemoryAndHeapThresholdsUseBytesWithoutRounding) {
    for (const auto delta : {-1, 0, 1}) {
        auto facts = HighDevice();
        facts.physical_memory_bytes = 8 * kGiB + delta;
        EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB),
                  delta > 0 ? DeviceQuality::kHigh : DeviceQuality::kLow);
        facts = HighDevice();
        EXPECT_EQ(ClassifyDevice(facts, 4 * kGiB + delta),
                  delta > 0 ? DeviceQuality::kHigh : DeviceQuality::kLow);
    }
    EXPECT_EQ(ClassifyDevice(HighDevice(), (std::numeric_limits<int64_t>::max)()), DeviceQuality::kHigh);
    for (const auto heap : {int64_t{0}, int64_t{-1}, (std::numeric_limits<int64_t>::min)()}) {
        EXPECT_EQ(ClassifyDevice(HighDevice(), heap), DeviceQuality::kUnknown);
        EXPECT_FALSE(QueryDeviceQuality(heap).ok());
    }
}

TEST(DeviceQualityTest, PhysicalCoresAndExistingSimdDetermineX86Tier) {
    auto facts = HighDevice();
    facts.physical_cores = 5;
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kLow);
    // Six physical cores cover both SMT and mixed P/E layouts; threads are not an input.
    facts.physical_cores = 6;
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kHigh);
    facts.x86_wide_simd = false;
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kLow);
#if YSM_X64 && !YSM_ANDROID
    InitCpuInfo();
    const auto original = simd::kSupported;
    for (const auto type : {simd::Type::kNone, simd::Type::SSE41,
                            simd::Type::AVX2, simd::Type::AVX512}) {
        simd::Mock(type);
        const auto probed = QueryDeviceFacts();
        ASSERT_TRUE(probed.ok()) << probed.status();
        EXPECT_EQ(probed->x86_wide_simd, type == simd::Type::AVX2 || type == simd::Type::AVX512);
        EXPECT_EQ(simd::kSupported, type);
    }
    simd::Mock(original);
#endif
}

TEST(DeviceQualityTest, ArmDoesNotRequireX86SimdOrSixCores) {
    auto facts = HighDevice();
    facts.architecture = DeviceArchitecture::kArm;
    facts.physical_cores = 2;
    facts.x86_wide_simd.reset();
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kHigh);
    facts.physical_cores.reset();
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kUnknown);
}

TEST(DeviceQualityTest, EveryApplicableMissingFactIsConservative) {
    for (int missing = 0; missing < 5; ++missing) {
        auto facts = HighDevice();
        switch (missing) {
            case 0: facts.platform = DevicePlatform::kUnknown; break;
            case 1: facts.architecture = DeviceArchitecture::kUnknown; break;
            case 2: facts.physical_memory_bytes.reset(); break;
            case 3: facts.physical_cores.reset(); break;
            case 4: facts.x86_wide_simd.reset(); break;
        }
        EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kUnknown) << missing;
    }
    auto facts = HighDevice();
    facts.physical_memory_bytes = 0;
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kUnknown);
    facts = HighDevice();
    facts.physical_cores = 0;
    EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kUnknown);
}

TEST(DeviceQualityTest, KnownLowConditionsDoNotRequireOtherProbes) {
    DeviceFacts facts;
    for (const auto platform : {DevicePlatform::kAndroid, DevicePlatform::kWindows7}) {
        facts.platform = platform;
        EXPECT_EQ(ClassifyDevice(facts, 8 * kGiB), DeviceQuality::kLow);
    }
    EXPECT_EQ(*QueryDeviceQuality(4 * kGiB), DeviceQuality::kLow);
}

#if YSM_WINDOWS
TEST(DeviceQualityTest, DeviceTotalDoesNotFollowProcessAffinity) {
    InitCpuInfo();
    const auto before = QueryDeviceFacts();
    ASSERT_TRUE(before.ok()) << before.status();
    const auto cores = QueryPhysicalCoreCount();
    ASSERT_TRUE(cores.ok()) << cores.status();
    EXPECT_EQ(*cores, before->physical_cores);
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    ASSERT_TRUE(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask));
    const auto one_cpu = process_mask & (~process_mask + 1);
    ASSERT_TRUE(SetProcessAffinityMask(GetCurrentProcess(), one_cpu));
    const auto restricted = QueryDeviceFacts();
    const auto restricted_cores = QueryPhysicalCoreCount();
    const auto restored = SetProcessAffinityMask(GetCurrentProcess(), process_mask);
    ASSERT_TRUE(restored);
    ASSERT_TRUE(restricted.ok()) << restricted.status();
    ASSERT_TRUE(restricted_cores.ok()) << restricted_cores.status();
    EXPECT_EQ(*restricted_cores, *cores);
    EXPECT_EQ(restricted->physical_cores, before->physical_cores);
    EXPECT_EQ(restricted->physical_memory_bytes, before->physical_memory_bytes);
}
#endif
}  // namespace
}  // namespace ysm
