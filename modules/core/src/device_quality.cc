#include "device_quality.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "log.h"

#if YSM_X64
#include "cpu.h"
#endif

#if YSM_WINDOWS
#include <windows.h>
#elif YSM_LINUX || YSM_ANDROID
#include <unistd.h>
#elif YSM_MACOS
#include <sys/sysctl.h>
#endif

namespace ysm {
namespace {
constexpr uint64_t kGiB = uint64_t{1} << 30;

absl::Status ProbeFailure() {
    return absl::UnavailableError("Device facts are unavailable or inconsistent");
}

#if YSM_WINDOWS
absl::StatusOr<DevicePlatform> QueryWindowsPlatform() {
    // Resolve the real version without the host executable's manifest policy.
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    using QueryVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto query = ntdll == nullptr ? nullptr : reinterpret_cast<QueryVersion>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (query == nullptr || query(&version) != 0 ||
        version.dwPlatformId != VER_PLATFORM_WIN32_NT ||
        version.dwMajorVersion < 6 ||
        (version.dwMajorVersion == 6 && version.dwMinorVersion < 1)) {
        return ProbeFailure();
    }
    return version.dwMajorVersion == 6 && version.dwMinorVersion == 1
        ? DevicePlatform::kWindows7 : DevicePlatform::kDesktop;
}

absl::StatusOr<uint32_t> QueryPhysicalCores() {
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return ProbeFailure();
    }
    std::vector<std::byte> bytes(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(bytes.data()),
            &length) || length == 0 || length > bytes.size()) {
        return ProbeFailure();
    }
    size_t offset = 0;
    uint32_t cores = 0;
    std::map<WORD, KAFFINITY> seen;
    constexpr auto kGroupsOffset = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                           Processor.GroupMask);
    while (offset < length) {
        if (length - offset < kGroupsOffset) return ProbeFailure();
        // Copy the fixed prefix before accessing any variable-length arrays.
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record{};
        std::memcpy(&record, bytes.data() + offset, kGroupsOffset);
        if (record.Relationship != RelationProcessorCore ||
            record.Size > length - offset || record.Size < kGroupsOffset ||
            record.Processor.GroupCount == 0 ||
            record.Processor.GroupCount >
                (record.Size - kGroupsOffset) / sizeof(GROUP_AFFINITY)) {
            return ProbeFailure();
        }
        for (size_t group = 0; group < record.Processor.GroupCount; ++group) {
            GROUP_AFFINITY affinity{};
            std::memcpy(&affinity, bytes.data() + offset + kGroupsOffset +
                        group * sizeof(affinity), sizeof(affinity));
            if (affinity.Mask == 0 || (seen[affinity.Group] & affinity.Mask) != 0) {
                return ProbeFailure();
            }
            seen[affinity.Group] |= affinity.Mask;
        }
        ++cores;
        offset += record.Size;
    }
    if (cores == 0) return ProbeFailure();
    return cores;
}
#elif YSM_LINUX || YSM_ANDROID
std::optional<std::string> ReadText(const std::string& path) {
    std::ifstream input(path);
    std::string text;
    if (!std::getline(input, text)) return std::nullopt;
    return text;
}

std::optional<uint32_t> ParseNumber(std::string_view text) {
    uint32_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<std::set<uint32_t>> ReadCpuList(const std::string& path) {
    auto text = ReadText(path);
    if (!text) return std::nullopt;
    std::string_view rest = *text;
    std::set<uint32_t> cpus;
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto item = rest.substr(0, comma);
        const auto dash = item.find('-');
        const auto first = ParseNumber(item.substr(0, dash));
        const auto last = dash == std::string_view::npos ? first : ParseNumber(item.substr(dash + 1));
        // Bound malformed ranges before expansion; supported systems are far smaller.
        if (!first || !last || *last < *first || *last >= 1'048'576) return std::nullopt;
        for (auto cpu = *first; cpu <= *last; ++cpu) {
            if (!cpus.insert(cpu).second) return std::nullopt;
        }
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
        if (rest.empty()) return std::nullopt;
    }
    if (cpus.empty()) return std::nullopt;
    return cpus;
}

absl::StatusOr<uint32_t> QueryPhysicalCores() {
    const auto present = ReadCpuList("/sys/devices/system/cpu/present");
    if (!present) return ProbeFailure();
    std::map<uint32_t, std::set<uint32_t>> groups;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> identities;
    for (const auto cpu : *present) {
        const auto base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        const auto siblings = ReadCpuList(base + "thread_siblings_list");
        const auto package_text = ReadText(base + "physical_package_id");
        const auto core_text = ReadText(base + "core_id");
        if (!siblings || !siblings->contains(cpu) || !package_text || !core_text ||
            !std::includes(present->begin(), present->end(), siblings->begin(), siblings->end())) {
            return ProbeFailure();
        }
        const auto package = ParseNumber(*package_text);
        const auto core = ParseNumber(*core_text);
        if (!package || !core) return ProbeFailure();
        const auto leader = *siblings->begin();
        const auto [group, inserted] = groups.emplace(leader, *siblings);
        const auto [identity, new_identity] = identities.emplace(std::pair(*package, *core), leader);
        if ((!inserted && group->second != *siblings) ||
            (!new_identity && identity->second != leader)) return ProbeFailure();
    }
    // Every sibling must report the same partition, including offline present CPUs.
    size_t members = 0;
    for (const auto& [leader, siblings] : groups) members += siblings.size();
    if (members != present->size() || identities.size() != groups.size()) return ProbeFailure();
    return static_cast<uint32_t>(groups.size());
}
#elif YSM_MACOS
template <typename T>
absl::StatusOr<T> ReadSysctl(const char* name) {
    T value{};
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0 ||
        size != sizeof(value) || value <= 0) return ProbeFailure();
    return value;
}
#endif
}  // namespace

absl::StatusOr<uint32_t> QueryPhysicalCoreCount() {
#if YSM_WINDOWS || YSM_LINUX || YSM_ANDROID
    return QueryPhysicalCores();
#elif YSM_MACOS
    const auto cores = ReadSysctl<int32_t>("hw.physicalcpu_max");
    if (!cores.ok()) return cores.status();
    return static_cast<uint32_t>(*cores);
#else
    return ProbeFailure();
#endif
}

DeviceQuality ClassifyDevice(const DeviceFacts& facts, int64_t max_heap_bytes) {
    if (max_heap_bytes <= 0) return DeviceQuality::kUnknown;
    if (facts.platform == DevicePlatform::kAndroid ||
        facts.platform == DevicePlatform::kWindows7 ||
        static_cast<uint64_t>(max_heap_bytes) <= 4 * kGiB) return DeviceQuality::kLow;
    if (facts.platform != DevicePlatform::kDesktop ||
        (facts.architecture != DeviceArchitecture::kX86 &&
         facts.architecture != DeviceArchitecture::kArm) ||
        !facts.physical_memory_bytes || *facts.physical_memory_bytes == 0 ||
        !facts.physical_cores || *facts.physical_cores == 0) return DeviceQuality::kUnknown;
    if (*facts.physical_memory_bytes <= 8 * kGiB) return DeviceQuality::kLow;
    if (facts.architecture == DeviceArchitecture::kX86) {
        if (!facts.x86_wide_simd) return DeviceQuality::kUnknown;
        if (!*facts.x86_wide_simd || *facts.physical_cores < 6) return DeviceQuality::kLow;
    }
    return DeviceQuality::kHigh;
}

absl::StatusOr<DeviceFacts> QueryDeviceFacts() {
    DeviceFacts facts;
#if YSM_ANDROID
    facts.platform = DevicePlatform::kAndroid;
    return facts;
#elif YSM_WINDOWS
    auto platform = QueryWindowsPlatform();
    if (!platform.ok()) return platform.status();
    facts.platform = *platform;
    if (facts.platform == DevicePlatform::kWindows7) return facts;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory) || memory.ullTotalPhys == 0) return ProbeFailure();
    facts.physical_memory_bytes = memory.ullTotalPhys;
    auto cores = QueryPhysicalCoreCount();
    if (!cores.ok()) return cores.status();
    facts.physical_cores = *cores;
#elif YSM_LINUX
    facts.platform = DevicePlatform::kDesktop;
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0 || static_cast<uint64_t>(pages) >
        std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(page_size)) return ProbeFailure();
    facts.physical_memory_bytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
    auto cores = QueryPhysicalCoreCount();
    if (!cores.ok()) return cores.status();
    facts.physical_cores = *cores;
#elif YSM_MACOS
    facts.platform = DevicePlatform::kDesktop;
    auto memory = ReadSysctl<uint64_t>("hw.memsize");
    auto cores = QueryPhysicalCoreCount();
    auto logical = ReadSysctl<int32_t>("hw.logicalcpu_max");
    if (!memory.ok() || !cores.ok() || !logical.ok() ||
        static_cast<uint32_t>(*logical) < *cores) return ProbeFailure();
    facts.physical_memory_bytes = *memory;
    facts.physical_cores = static_cast<uint32_t>(*cores);
#else
    return ProbeFailure();
#endif
#if YSM_X64
    facts.architecture = DeviceArchitecture::kX86;
    facts.x86_wide_simd = simd::kSupported == simd::Type::AVX2 ||
                          simd::kSupported == simd::Type::AVX512;
#elif YSM_ARM64
    facts.architecture = DeviceArchitecture::kArm;
#endif
    YSM_LOG(INFO, "Device facts: physical memory {} bytes, physical cores {}",
            facts.physical_memory_bytes.value_or(0), facts.physical_cores.value_or(0));
    return facts;
}

absl::StatusOr<DeviceQuality> QueryDeviceQuality(int64_t max_heap_bytes) {
    if (max_heap_bytes <= 0) return absl::InvalidArgumentError("Maximum heap must be positive bytes");
    if (static_cast<uint64_t>(max_heap_bytes) <= 4 * kGiB) return DeviceQuality::kLow;
    const auto facts = QueryDeviceFacts();
    if (!facts.ok()) return facts.status();
    const auto quality = ClassifyDevice(*facts, max_heap_bytes);
    if (quality == DeviceQuality::kUnknown) return ProbeFailure();
    return quality;
}

}  // namespace ysm
