#include "cpu_topology.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "log.h"

#if YSM_WINDOWS
#include <windows.h>
#elif YSM_LINUX
#include <cerrno>
#include <sched.h>
#elif YSM_MACOS
#include <sys/sysctl.h>
#endif

namespace ysm {
namespace {
#if YSM_WINDOWS || YSM_LINUX
std::shared_ptr<std::atomic_bool> MakeFailureFlag() {
    return std::make_shared<std::atomic_bool>(false);
}
#endif

#if YSM_LINUX
std::optional<std::string> ReadTextFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::string value;
    std::getline(input, value);
    return value;
}

std::optional<unsigned int> ParseUnsigned(std::string_view text) {
    unsigned int value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::set<unsigned int>> ParseCpuList(std::string_view text) {
    std::set<unsigned int> cpus;
    while (!text.empty()) {
        const auto comma = text.find(',');
        auto item = text.substr(0, comma);
        const auto dash = item.find('-');
        auto first = ParseUnsigned(item.substr(0, dash));
        if (!first) {
            return std::nullopt;
        }
        auto last = first;
        if (dash != std::string_view::npos) {
            last = ParseUnsigned(item.substr(dash + 1));
            if (!last || *last < *first) {
                return std::nullopt;
            }
        }
        for (auto cpu = *first;; ++cpu) {
            cpus.insert(cpu);
            if (cpu == *last) {
                break;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return cpus.empty() ? std::nullopt
                        : std::optional<std::set<unsigned int>>(cpus);
}

std::optional<std::set<unsigned int>> ReadCpuList(const std::string& path) {
    const auto text = ReadTextFile(path);
    return text ? ParseCpuList(*text) : std::nullopt;
}

std::optional<unsigned int> ReadUnsignedFile(const std::string& path) {
    const auto text = ReadTextFile(path);
    return text ? ParseUnsigned(*text) : std::nullopt;
}

std::set<unsigned int> Intersect(const std::set<unsigned int>& left,
                                 const std::set<unsigned int>& right) {
    std::set<unsigned int> result;
    std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                          std::inserter(result, result.end()));
    return result;
}

std::optional<unsigned int> PhysicalCoreKey(unsigned int cpu) {
    const auto siblings = ReadCpuList(
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/thread_siblings_list");
    if (!siblings || siblings->empty()) {
        return std::nullopt;
    }
    return *siblings->begin();
}
#elif YSM_MACOS
std::optional<size_t> ReadSysctlSize(const char* name) {
    uint64_t value = 0;
    size_t value_size = sizeof(value);
    if (sysctlbyname(name, &value, &value_size, nullptr, 0) != 0 ||
        value_size == 0 || value == 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(value);
}
#endif
}  // namespace

std::optional<CpuTopology> DetectCpuTopology() {
#if YSM_WINDOWS
    using QueryCpuSets = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG,
                                      PULONG, HANDLE, ULONG);
    using BindCpuSets = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);

    // Kernel32 stays loaded for the process lifetime; CPU Sets are optional
    // because the library must also load on Windows 7.
    const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        return std::nullopt;
    }
    const auto query_cpu_sets = reinterpret_cast<QueryCpuSets>(
        GetProcAddress(kernel32, "GetSystemCpuSetInformation"));
    const auto bind_cpu_sets = reinterpret_cast<BindCpuSets>(
        GetProcAddress(kernel32, "SetThreadSelectedCpuSets"));
    if (query_cpu_sets == nullptr || bind_cpu_sets == nullptr) {
        return std::nullopt;
    }

    ULONG buffer_size = 0;
    if (query_cpu_sets(nullptr, 0, &buffer_size, GetCurrentProcess(), 0) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer_size == 0) {
        return std::nullopt;
    }

    std::vector<std::byte> buffer(buffer_size);
    if (!query_cpu_sets(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
            buffer_size, &buffer_size, GetCurrentProcess(), 0)) {
        return std::nullopt;
    }

    struct LogicalCpu {
        ULONG id;
        uint32_t core_key;
        uint8_t efficiency_class;
    };
    std::vector<LogicalCpu> logical_cpus;
    std::map<uint32_t, uint8_t> core_classes;
    size_t offset = 0;
    while (offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= buffer_size) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(
            buffer.data() + offset);
        if (info->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) ||
            info->Size > buffer_size - offset) {
            return std::nullopt;
        }
        offset += info->Size;
        if (info->Type != CpuSetInformation || info->CpuSet.Parked ||
            (info->CpuSet.Allocated &&
             !info->CpuSet.AllocatedToTargetProcess)) {
            continue;
        }

        const auto core_key =
            (static_cast<uint32_t>(info->CpuSet.Group) << 8U) |
            info->CpuSet.CoreIndex;
        logical_cpus.push_back({info->CpuSet.Id, core_key,
                                info->CpuSet.EfficiencyClass});
        core_classes.insert_or_assign(core_key,
                                      info->CpuSet.EfficiencyClass);
    }
    if (offset != buffer_size || logical_cpus.empty() ||
        core_classes.empty()) {
        return std::nullopt;
    }

    std::set<uint8_t> efficiency_classes;
    for (const auto& [core_key, efficiency_class] : core_classes) {
        static_cast<void>(core_key);
        efficiency_classes.insert(efficiency_class);
    }

    CpuTopology result;
    result.logical_core_count = logical_cpus.size();
    result.physical_core_count = core_classes.size();
    result.has_smt = logical_cpus.size() > core_classes.size();
    result.hybrid = efficiency_classes.size() > 1;
    if (!result.hybrid) {
        return result;
    }

    const auto performance_class = *efficiency_classes.rbegin();
    std::set<uint32_t> performance_cores;
    std::vector<ULONG> performance_cpu_sets;
    for (const auto& cpu : logical_cpus) {
        if (cpu.efficiency_class == performance_class) {
            performance_cores.insert(cpu.core_key);
            performance_cpu_sets.push_back(cpu.id);
        }
    }
    if (performance_cores.empty() || performance_cpu_sets.empty()) {
        return std::nullopt;
    }

    result.performance_core_count = performance_cores.size();
    auto failure_reported = MakeFailureFlag();
    result.bind_current_thread_to_performance_cores =
        [cpu_sets = std::move(performance_cpu_sets), failure_reported,
         bind_cpu_sets] {
            if (bind_cpu_sets(
                    GetCurrentThread(), cpu_sets.data(),
                    static_cast<ULONG>(cpu_sets.size()))) {
                return;
            }
            const auto error = GetLastError();
            if (!failure_reported->exchange(true,
                                            std::memory_order_relaxed)) {
                YSM_LOG(WARNING,
                        "Failed to bind current thread to performance "
                        "cores: Windows error {}",
                        error);
            }
        };
    return result;
#elif YSM_LINUX
    cpu_set_t allowed_mask;
    CPU_ZERO(&allowed_mask);
    if (sched_getaffinity(0, sizeof(allowed_mask), &allowed_mask) != 0) {
        return std::nullopt;
    }

    std::set<unsigned int> allowed_cpus;
    for (unsigned int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed_mask)) {
            allowed_cpus.insert(cpu);
        }
    }
    if (allowed_cpus.empty()) {
        return std::nullopt;
    }

    std::map<unsigned int, unsigned int> physical_keys;
    std::set<unsigned int> physical_cores;
    for (const auto cpu : allowed_cpus) {
        auto key = PhysicalCoreKey(cpu);
        if (!key) {
            return std::nullopt;
        }
        physical_keys.emplace(cpu, *key);
        physical_cores.insert(*key);
    }

    CpuTopology result;
    result.logical_core_count = allowed_cpus.size();
    result.physical_core_count = physical_cores.size();
    result.has_smt = allowed_cpus.size() > physical_cores.size();

    std::set<unsigned int> performance_cpus;
    const auto core_cpus = ReadCpuList("/sys/devices/cpu_core/cpus");
    const auto atom_cpus = ReadCpuList("/sys/devices/cpu_atom/cpus");
    const auto low_power_cpus =
        ReadCpuList("/sys/devices/cpu_lowpower/cpus");
    if (core_cpus) {
        const auto visible_core_cpus = Intersect(*core_cpus, allowed_cpus);
        std::set<unsigned int> visible_efficiency_cpus;
        if (atom_cpus) {
            visible_efficiency_cpus = Intersect(*atom_cpus, allowed_cpus);
        }
        if (low_power_cpus) {
            const auto visible_low_power =
                Intersect(*low_power_cpus, allowed_cpus);
            visible_efficiency_cpus.insert(visible_low_power.begin(),
                                           visible_low_power.end());
        }
        if (!visible_core_cpus.empty() &&
            !visible_efficiency_cpus.empty()) {
            performance_cpus = visible_core_cpus;
        }
    }

    if (performance_cpus.empty()) {
        std::map<unsigned int, unsigned int> capacities;
        std::set<unsigned int> capacity_classes;
        for (const auto cpu : allowed_cpus) {
            const auto capacity = ReadUnsignedFile(
                "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                "/cpu_capacity");
            if (!capacity) {
                capacities.clear();
                break;
            }
            capacities.emplace(cpu, *capacity);
            capacity_classes.insert(*capacity);
        }
        if (!capacities.empty() && capacity_classes.size() > 1) {
            const auto performance_capacity = *capacity_classes.rbegin();
            for (const auto [cpu, capacity] : capacities) {
                if (capacity == performance_capacity) {
                    performance_cpus.insert(cpu);
                }
            }
        }
    }

    if (performance_cpus.empty()) {
        return result;
    }

    result.hybrid = true;
    std::set<unsigned int> performance_cores;
    cpu_set_t performance_mask;
    CPU_ZERO(&performance_mask);
    for (const auto cpu : performance_cpus) {
        performance_cores.insert(physical_keys.at(cpu));
        CPU_SET(cpu, &performance_mask);
    }
    result.performance_core_count = performance_cores.size();
    auto failure_reported = MakeFailureFlag();
    result.bind_current_thread_to_performance_cores =
        [performance_mask, failure_reported] {
            if (sched_setaffinity(0, sizeof(performance_mask),
                                  &performance_mask) == 0) {
                return;
            }
            const auto error = errno;
            if (!failure_reported->exchange(true,
                                            std::memory_order_relaxed)) {
                YSM_LOG(WARNING,
                        "Failed to bind current thread to performance "
                        "cores: errno {}",
                        error);
            }
        };
    return result;
#elif YSM_MACOS
    const auto logical_core_count = ReadSysctlSize("hw.logicalcpu");
    const auto physical_core_count = ReadSysctlSize("hw.physicalcpu");
    if (!logical_core_count || !physical_core_count) {
        return std::nullopt;
    }

    CpuTopology result;
    result.logical_core_count = *logical_core_count;
    result.physical_core_count = *physical_core_count;
    result.has_smt = *logical_core_count > *physical_core_count;

    const auto performance_level_count = ReadSysctlSize("hw.nperflevels");
    if (performance_level_count && *performance_level_count > 1) {
        const auto performance_core_count =
            ReadSysctlSize("hw.perflevel0.physicalcpu");
        if (!performance_core_count) {
            return std::nullopt;
        }
        result.hybrid = true;
        result.performance_core_count = *performance_core_count;
    }
    return result;
#else
    return std::nullopt;
#endif
}
}  // namespace ysm
