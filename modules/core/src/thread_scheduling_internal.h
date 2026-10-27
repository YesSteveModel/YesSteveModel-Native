#pragma once

#if YSM_WINDOWS
#include <absl/functional/function_ref.h>
#endif

#include "thread_scheduling.h"

namespace ysm::thread_scheduling_internal {
constexpr ThreadPolicyOutcome ClassifyQosReadback(int requested_class,
                                                 int observed_class,
                                                 int relative_priority) {
    if (observed_class == requested_class && relative_priority == 0) {
        return ThreadPolicyOutcome::kApplied;
    }
    if (observed_class != 0 && observed_class <= requested_class && relative_priority <= 0) {
        return ThreadPolicyOutcome::kDegraded;
    }
    return ThreadPolicyOutcome::kFailed;
}

#if YSM_WINDOWS
struct WindowsCall {
    bool ok = false;
    int64_t value = 0;
    uint32_t error = 0;
};

// Borrowed for one application; the OS Adapter and fault-injection Adapter share
// the same policy path without process-wide hooks or retained thread handles.
struct WindowsApi {
    bool memory_api_available;
    absl::FunctionRef<WindowsCall()> query_cpu;
    absl::FunctionRef<WindowsCall()> query_memory;
    absl::FunctionRef<WindowsCall(int)> set_priority;
    absl::FunctionRef<WindowsCall()> set_normal_memory;
};

ThreadPolicyReport ApplyWindowsThreadPolicy(ThreadPolicy policy,
                                           const WindowsApi& api);
#endif
}  // namespace ysm::thread_scheduling_internal
