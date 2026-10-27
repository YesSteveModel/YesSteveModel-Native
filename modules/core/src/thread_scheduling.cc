#include "thread_scheduling.h"

#include <algorithm>
#include <cerrno>

#include "thread_scheduling_internal.h"

#if YSM_WINDOWS
#include <windows.h>
#elif YSM_LINUX || YSM_ANDROID
#include <sched.h>
#include <sys/resource.h>
#elif YSM_MACOS
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ysm {
namespace {
using Outcome = ThreadPolicyOutcome;
using Operation = ThreadPolicyOperation;
using Result = ThreadPolicyStepResult;
using Domain = ThreadPolicyErrorDomain;
using Attribute = ThreadPolicyAttribute;

ThreadPolicyPlatform CurrentPlatform() {
#if YSM_WINDOWS
    return ThreadPolicyPlatform::kWindows;
#elif YSM_ANDROID
    return ThreadPolicyPlatform::kAndroid;
#elif YSM_LINUX
    return ThreadPolicyPlatform::kLinux;
#elif YSM_MACOS
    return ThreadPolicyPlatform::kMacos;
#endif
}

void AddStep(ThreadPolicyReport& report, Operation operation, Result result,
             Domain domain = Domain::kNone, int64_t error = 0) {
    report.steps.at(report.step_count++) = {operation, result, domain, error};
}

void Observe(ThreadPolicyReport& report, Attribute attribute, int64_t value) {
    const auto index = static_cast<size_t>(attribute);
    report.observed_mask |= uint64_t{1} << index;
    report.observed[index] = value;
}

#if YSM_WINDOWS
using thread_scheduling_internal::WindowsApi;
using thread_scheduling_internal::WindowsCall;

bool Record(ThreadPolicyReport& report, Operation operation, WindowsCall call) {
    AddStep(report, operation, call.ok ? Result::kOk : Result::kFailed,
            call.ok ? Domain::kNone : Domain::kWin32,
            call.ok ? 0 : static_cast<int64_t>(call.error));
    return call.ok;
}

bool QueryWindowsAfter(ThreadPolicyReport& report, const WindowsApi& api,
                       bool memory) {
    const auto cpu = api.query_cpu();
    const auto cpu_ok = Record(report, Operation::kQueryAfter, cpu);
    if (cpu_ok) Observe(report, Attribute::kCpu, cpu.value);
    if (!memory) return cpu_ok;
    const auto mem = api.query_memory();
    const auto memory_ok = Record(report, Operation::kQueryAfter, mem);
    if (memory_ok) Observe(report, Attribute::kMemory, mem.value);
    return cpu_ok && memory_ok;
}

ThreadPolicyReport ApplyWindows(ThreadPolicy policy, const WindowsApi& api) {
    ThreadPolicyReport report;
    report.platform = ThreadPolicyPlatform::kWindows;
    if (policy == ThreadPolicy::kNormal) {
        report.outcome = Outcome::kUnchanged;
        return report;
    }
    if (policy != ThreadPolicy::kBelowNormal && policy != ThreadPolicy::kLow) return report;

    const auto before = api.query_cpu();
    if (!Record(report, Operation::kQueryBefore, before)) return report;

    if (policy == ThreadPolicy::kLow && before.value < THREAD_PRIORITY_LOWEST) {
        // BEGIN can raise an inherited IDLE priority to the background base.
        // Preserve the host policy without claiming ownership of its mode.
        AddStep(report, Operation::kBeginBackground, Result::kSkipped);
        AddStep(report, Operation::kSetCpu, Result::kSkipped);
        QueryWindowsAfter(report, api, api.memory_api_available);
        report.outcome = (report.observed_mask & 1) && report.observed[0] <= before.value
                             ? Outcome::kDegraded : Outcome::kFailed;
        return report;
    }

    bool changed = false;
    bool fallback = false;
    bool cpu_ok = true;
    if (policy == ThreadPolicy::kLow && api.memory_api_available) {
        const auto begin = api.set_priority(THREAD_MODE_BACKGROUND_BEGIN);
        if (Record(report, Operation::kBeginBackground, begin)) {
            changed = true;
            if (Record(report, Operation::kSetMemory, api.set_normal_memory())) {
                const auto verified = QueryWindowsAfter(report, api, true);
                report.outcome = verified && report.observed[0] <= before.value &&
                                         report.observed[2] == 5
                                     ? Outcome::kApplied : Outcome::kPartial;
                return report;
            }
            // Only a successful BEGIN in this call grants rollback ownership.
            if (!Record(report, Operation::kEndBackground,
                        api.set_priority(THREAD_MODE_BACKGROUND_END))) {
                QueryWindowsAfter(report, api, true);
                report.outcome = Outcome::kPartial;
                return report;
            }
            changed = false;
        }
        fallback = true;
    } else if (policy == ThreadPolicy::kLow) {
        AddStep(report, Operation::kSetMemory, Result::kUnsupported);
        fallback = true;
    }

    const auto target = policy == ThreadPolicy::kBelowNormal
                            ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    if (before.value > target) {
        cpu_ok = Record(report, Operation::kSetCpu, api.set_priority(target));
        changed = cpu_ok;
    } else {
        AddStep(report, Operation::kSetCpu, Result::kSkipped);
    }
    const auto verified = QueryWindowsAfter(report, api, false);
    if (cpu_ok && verified && report.observed[0] <= target) {
        report.outcome = fallback ? Outcome::kDegraded : Outcome::kApplied;
    } else {
        report.outcome = changed ? Outcome::kPartial : Outcome::kFailed;
    }
    return report;
}

ThreadPolicyReport ApplyWindowsOs(ThreadPolicy policy) {
    const auto thread = GetCurrentThread();
    // The optional APIs start at Windows 8; importing them would break Win7 load.
    using ThreadInformation = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto get_info = reinterpret_cast<ThreadInformation>(
        kernel ? GetProcAddress(kernel, "GetThreadInformation") : nullptr);
    const auto set_info = reinterpret_cast<ThreadInformation>(
        kernel ? GetProcAddress(kernel, "SetThreadInformation") : nullptr);
    struct MemoryPriority { ULONG value; };
    auto query_cpu = [thread] {
        const auto value = GetThreadPriority(thread);
        if (value == THREAD_PRIORITY_ERROR_RETURN) return WindowsCall{false, 0, GetLastError()};
        return WindowsCall{true, value};
    };
    auto query_memory = [=] {
        MemoryPriority memory{};
        if (!get_info(thread, ThreadMemoryPriority, &memory, sizeof(memory))) {
            return WindowsCall{false, 0, GetLastError()};
        }
        return WindowsCall{true, memory.value};
    };
    auto set_priority = [thread](int value) {
        if (!SetThreadPriority(thread, value)) return WindowsCall{false, 0, GetLastError()};
        return WindowsCall{true};
    };
    auto set_normal_memory = [=] {
        MemoryPriority memory{5};
        if (!set_info(thread, ThreadMemoryPriority, &memory, sizeof(memory))) {
            return WindowsCall{false, 0, GetLastError()};
        }
        return WindowsCall{true};
    };
    return ApplyWindows(policy, {get_info && set_info, query_cpu, query_memory,
                                 set_priority, set_normal_memory});
}
#elif YSM_LINUX || YSM_ANDROID
bool RecordErrno(ThreadPolicyReport& report, Operation operation, bool ok, int error) {
    AddStep(report, operation, ok ? Result::kOk : Result::kFailed,
            ok ? Domain::kNone : Domain::kErrno, ok ? 0 : error);
    return ok;
}

struct PosixSnapshot {
    int nice;
    int scheduler;
    bool valid;
};

PosixSnapshot QueryPosix(ThreadPolicyReport& report, Operation operation) {
    errno = 0;
    const auto nice = getpriority(PRIO_PROCESS, 0);
    const auto nice_error = errno;
    const auto nice_ok = RecordErrno(report, operation, nice_error == 0, nice_error);
    const auto scheduler = sched_getscheduler(0);
    const auto scheduler_error = errno;
    const auto scheduler_ok = RecordErrno(report, operation, scheduler != -1, scheduler_error);
    if (operation == Operation::kQueryAfter) {
        if (nice_ok) Observe(report, Attribute::kCpu, nice);
        if (scheduler_ok) Observe(report, Attribute::kScheduler, scheduler);
    }
    return {nice, scheduler, nice_ok && scheduler_ok};
}

ThreadPolicyReport ApplyPosix(ThreadPolicy policy) {
    ThreadPolicyReport report;
    report.platform = CurrentPlatform();
    const auto before = QueryPosix(report, Operation::kQueryBefore);
    if (!before.valid) return report;

    const auto target = policy == ThreadPolicy::kBelowNormal ? 5 : 10;
    bool nice_ok = true;
    bool changed = false;
    if (before.nice < target) {
        const auto ok = setpriority(PRIO_PROCESS, 0, target) == 0;
        nice_ok = RecordErrno(report, Operation::kSetCpu, ok, errno);
        changed = nice_ok;
    } else {
        AddStep(report, Operation::kSetCpu, Result::kSkipped);
    }
    bool scheduler_ok = true;
    if (policy == ThreadPolicy::kLow) {
        if (before.scheduler == SCHED_OTHER) {
            const sched_param param{0};
            const auto ok = sched_setscheduler(0, SCHED_BATCH, &param) == 0;
            scheduler_ok = RecordErrno(report, Operation::kSetScheduler, ok, errno);
            changed |= scheduler_ok;
        } else {
            // Never move an externally assigned idle or realtime thread into BATCH.
            AddStep(report, Operation::kSetScheduler, Result::kSkipped);
            scheduler_ok = before.scheduler == SCHED_BATCH || before.scheduler == SCHED_IDLE;
        }
    }
    const auto after = QueryPosix(report, Operation::kQueryAfter);
    if (!after.valid) {
        report.outcome = changed ? Outcome::kPartial : Outcome::kFailed;
    } else if (nice_ok && after.nice >= target) {
        const auto full_scheduler = policy == ThreadPolicy::kBelowNormal ||
                                    (scheduler_ok && (after.scheduler == SCHED_BATCH ||
                                                       after.scheduler == SCHED_IDLE));
        report.outcome = full_scheduler ? Outcome::kApplied : Outcome::kDegraded;
    } else {
        report.outcome = changed ? Outcome::kPartial : Outcome::kFailed;
    }
    return report;
}
#elif YSM_MACOS
bool RecordPthread(ThreadPolicyReport& report, Operation operation, int error) {
    AddStep(report, operation, error == 0 ? Result::kOk : Result::kFailed,
            error == 0 ? Domain::kNone : Domain::kPthread, error);
    return error == 0;
}

ThreadPolicyReport ApplyMacos(ThreadPolicy policy) {
    ThreadPolicyReport report;
    report.platform = ThreadPolicyPlatform::kMacos;
    qos_class_t before = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    if (!RecordPthread(report, Operation::kQueryBefore,
                       pthread_get_qos_class_np(pthread_self(), &before, &relative))) return report;
    const auto target = policy == ThreadPolicy::kBelowNormal
                            ? QOS_CLASS_UTILITY : QOS_CLASS_BACKGROUND;
    bool changed = false;
    bool set_ok = true;
    if (before != QOS_CLASS_UNSPECIFIED && before <= target) {
        AddStep(report, Operation::kSetQos, Result::kSkipped);
    } else {
        set_ok = RecordPthread(report, Operation::kSetQos,
                               pthread_set_qos_class_self_np(target, 0));
        changed = set_ok;
    }
    qos_class_t after = QOS_CLASS_UNSPECIFIED;
    int after_relative = 0;
    const auto verified = RecordPthread(report, Operation::kQueryAfter,
        pthread_get_qos_class_np(pthread_self(), &after, &after_relative));
    if (verified) {
        Observe(report, Attribute::kQos, after);
        Observe(report, Attribute::kQosRelative, after_relative);
    }
    const auto readback = thread_scheduling_internal::ClassifyQosReadback(
        target, after, after_relative);
    report.outcome = set_ok && verified && readback != Outcome::kFailed
                         ? readback : changed ? Outcome::kPartial : Outcome::kFailed;
    return report;
}
#endif
}  // namespace

std::array<int64_t, ThreadPolicyReport::kMaxEncodedSize> ThreadPolicyReport::Encode() const {
    std::array<int64_t, kMaxEncodedSize> values{};
    values[0] = static_cast<int64_t>(outcome);
    values[1] = static_cast<int64_t>(platform);
    values[2] = static_cast<int64_t>(observed_mask);
    for (size_t i = 0; i < observed.size(); ++i) {
        if (observed_mask & (uint64_t{1} << i)) values[3 + i] = observed[i];
    }
    values[8] = static_cast<int64_t>(step_count);
    for (size_t i = 0; i < step_count; ++i) {
        const auto& step = steps[i];
        values[9 + 4 * i] = static_cast<int64_t>(step.operation);
        values[10 + 4 * i] = static_cast<int64_t>(step.result);
        values[11 + 4 * i] = static_cast<int64_t>(step.error_domain);
        values[12 + 4 * i] = step.native_error;
    }
    return values;
}

ThreadPolicyReport ApplyCurrentThreadPolicy(ThreadPolicy policy) {
    if (policy == ThreadPolicy::kNormal) {
        ThreadPolicyReport report;
        report.platform = CurrentPlatform();
        report.outcome = Outcome::kUnchanged;
        return report;
    }
    if (policy != ThreadPolicy::kBelowNormal && policy != ThreadPolicy::kLow) {
        ThreadPolicyReport report;
        report.platform = CurrentPlatform();
        return report;
    }
#if YSM_WINDOWS
    return ApplyWindowsOs(policy);
#elif YSM_LINUX || YSM_ANDROID
    return ApplyPosix(policy);
#elif YSM_MACOS
    return ApplyMacos(policy);
#endif
}

#if YSM_WINDOWS
ThreadPolicyReport thread_scheduling_internal::ApplyWindowsThreadPolicy(
    ThreadPolicy policy, const WindowsApi& api) {
    return ApplyWindows(policy, api);
}
#endif
}  // namespace ysm
