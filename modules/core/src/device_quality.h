#pragma once

#include <cstdint>
#include <optional>

#include <absl/status/statusor.h>

namespace ysm {

enum class DeviceQuality : int32_t { kUnknown = 0, kLow = 1, kHigh = 2 };
enum class DevicePlatform : uint8_t { kUnknown, kDesktop, kWindows7, kAndroid };
enum class DeviceArchitecture : uint8_t { kUnknown, kX86, kArm };

struct DeviceFacts {
    DevicePlatform platform = DevicePlatform::kUnknown;
    DeviceArchitecture architecture = DeviceArchitecture::kUnknown;
    std::optional<uint64_t> physical_memory_bytes;
    std::optional<uint32_t> physical_cores;
    std::optional<bool> x86_wide_simd;
};

// The facts describe the device, independently of renderer affinity and parking.
DeviceQuality ClassifyDevice(const DeviceFacts& facts, int64_t max_heap_bytes);
absl::StatusOr<uint32_t> QueryPhysicalCoreCount();
absl::StatusOr<DeviceFacts> QueryDeviceFacts();
absl::StatusOr<DeviceQuality> QueryDeviceQuality(int64_t max_heap_bytes);

}  // namespace ysm
