#include "parallel_executor.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "cpu_topology.h"

#ifdef YSM_WINDOWS
#include <windows.h>

#undef max

namespace ysm::gfx::renderer {
namespace {
struct AddressWaitApi {
    decltype(&WaitOnAddress) wait;
    decltype(&WakeByAddressSingle) notify_one;
    decltype(&WakeByAddressAll) notify_all;
};

const AddressWaitApi& GetAddressWaitApi() noexcept {
    // All operations must use the same backend, including concurrent first use.
    static const auto api = []() noexcept -> AddressWaitApi {
        // KernelBase remains loaded for the process lifetime. These optional
        // exports must not become a Windows 7 DLL loading requirement.
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (module == nullptr) {
            return {};
        }
        AddressWaitApi resolved{
            reinterpret_cast<decltype(&WaitOnAddress)>(
                GetProcAddress(module, "WaitOnAddress")),
            reinterpret_cast<decltype(&WakeByAddressSingle)>(
                GetProcAddress(module, "WakeByAddressSingle")),
            reinterpret_cast<decltype(&WakeByAddressAll)>(
                GetProcAddress(module, "WakeByAddressAll"))};
        if (resolved.wait == nullptr || resolved.notify_one == nullptr ||
            resolved.notify_all == nullptr) {
            return {};
        }
        return resolved;
    }();
    return api;
}
}  // namespace

void ParallelExecutor::NotifyOne(std::atomic_size_t &value) noexcept {
    if (const auto& api = GetAddressWaitApi(); api.wait != nullptr) [[likely]] {
        api.notify_one(&value);
    } else {
        value.notify_one();
    }
}

void ParallelExecutor::NotifyAll(std::atomic_size_t &value) noexcept {
    if (const auto& api = GetAddressWaitApi(); api.wait != nullptr) [[likely]] {
        api.notify_all(&value);
    } else {
        value.notify_all();
    }
}

void ParallelExecutor::WaitFor(std::atomic_size_t &value, size_t v) noexcept {
    static_assert(sizeof(value) == sizeof(v));
    const auto& api = GetAddressWaitApi();
    if (api.wait == nullptr) [[unlikely]] {
        value.wait(v, std::memory_order_acquire);
        return;
    }
    while (true) {
        api.wait(&value, &v, sizeof(v), INFINITE);
        if (value.load(std::memory_order_acquire) != v) [[likely]] {
            break;
        }
    }
}
}

#else

namespace ysm::gfx::renderer {
void ParallelExecutor::NotifyOne(std::atomic_size_t &value) noexcept {
    value.notify_one();
}

void ParallelExecutor::NotifyAll(std::atomic_size_t &value) noexcept {
    value.notify_all();
}

void ParallelExecutor::WaitFor(std::atomic_size_t &value, size_t v) noexcept {
    value.wait(v, std::memory_order_acquire);
}
}

#endif

namespace ysm::gfx::renderer {
namespace {
constexpr size_t kMinimumThreadCount = 2;
constexpr size_t kMaximumThreadCount = 8;

size_t ClampThreadCount(size_t value) noexcept {
    return std::clamp(value, kMinimumThreadCount, kMaximumThreadCount);
}

size_t CalculateDesktopThreadCount(const CpuTopology& topology) noexcept {
    if (topology.hybrid) {
        const auto available = topology.performance_core_count > 0
                                   ? topology.performance_core_count - 1
                                   : 0;
        return ClampThreadCount(available);
    }
    if (topology.has_smt) {
        const auto physical_equivalent = topology.logical_core_count / 2;
        const auto available =
            physical_equivalent > 2 ? physical_equivalent - 2 : 0;
        return ClampThreadCount(available);
    }
    const auto available = (topology.physical_core_count / 3) * 2 +
                           (topology.physical_core_count % 3) * 2 / 3;
    return ClampThreadCount(available);
}

size_t CalculateAndroidThreadCount(
    unsigned int logical_core_count) noexcept {
    const auto third = logical_core_count / 3;
    const auto available =
        third + static_cast<unsigned int>(logical_core_count % 3 != 0);
    return ClampThreadCount(available);
}

size_t CalculateFallbackThreadCount(
    unsigned int logical_core_count) noexcept {
    return ClampThreadCount(logical_core_count / 3);
}

struct ParallelExecutorConfig {
    size_t thread_count;
    ParallelExecutor::InitTask worker_init;
};

ParallelExecutorConfig CreateParallelExecutorConfig() {
    const auto hardware_threads = std::thread::hardware_concurrency();
#if YSM_ANDROID
    return {CalculateAndroidThreadCount(hardware_threads), {}};
#else
    auto topology = DetectCpuTopology();
    if (!topology) {
        return {CalculateFallbackThreadCount(hardware_threads), {}};
    }

    ParallelExecutor::InitTask worker_init;
    if (topology->bind_current_thread_to_performance_cores) {
        worker_init =
            [bind = std::move(
                 topology->bind_current_thread_to_performance_cores)](
                size_t, size_t) { bind(); };
    }
    return {CalculateDesktopThreadCount(*topology),
            std::move(worker_init)};
#endif
}
}

ParallelExecutor& ParallelExecutor::Get() noexcept {
    static const auto instance = [] {
        auto config = CreateParallelExecutorConfig();
        return std::make_unique<ParallelExecutor>(config.thread_count,
                                                  config.worker_init);
    }();
    return *instance;
}
  // namespace
}
