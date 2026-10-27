#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ysm {
enum class ThreadPolicy : int32_t { kNormal = 0, kBelowNormal = 1, kLow = 2 };
enum class ThreadPolicyOutcome : int64_t {
    kUnchanged = 0, kApplied = 1, kDegraded = 2, kPartial = 3,
    kFailed = 4, kUnsupported = 5,
};
enum class ThreadPolicyPlatform : int64_t {
    kWindows = 1, kLinux = 2, kAndroid = 3, kMacos = 4,
};
enum class ThreadPolicyOperation : int64_t {
    kQueryBefore = 1, kSetCpu = 2, kBeginBackground = 3, kSetMemory = 4,
    kEndBackground = 5, kSetScheduler = 6, kSetQos = 7, kQueryAfter = 8,
};
enum class ThreadPolicyStepResult : int64_t {
    kOk = 0, kSkipped = 1, kUnsupported = 2, kFailed = 3,
};
enum class ThreadPolicyErrorDomain : int64_t {
    kNone = 0, kWin32 = 1, kErrno = 2, kPthread = 3,
};
enum class ThreadPolicyAttribute : uint8_t {
    kCpu = 0, kScheduler = 1, kMemory = 2, kQos = 3, kQosRelative = 4,
};

struct ThreadPolicyStep {
    ThreadPolicyOperation operation{};
    ThreadPolicyStepResult result{};
    ThreadPolicyErrorDomain error_domain{};
    int64_t native_error = 0;
};

struct ThreadPolicyReport {
    static constexpr size_t kMaxSteps = 8;
    static constexpr size_t kHeaderSize = 9;
    static constexpr size_t kMaxEncodedSize = kHeaderSize + 4 * kMaxSteps;

    ThreadPolicyOutcome outcome = ThreadPolicyOutcome::kFailed;
    ThreadPolicyPlatform platform{};
    uint64_t observed_mask = 0;
    std::array<int64_t, 5> observed{};
    size_t step_count = 0;
    std::array<ThreadPolicyStep, kMaxSteps> steps{};

    size_t EncodedSize() const { return kHeaderSize + 4 * step_count; }
    std::array<int64_t, kMaxEncodedSize> Encode() const;
};

// Changes only the calling thread. Normal does not query or reset inherited state.
ThreadPolicyReport ApplyCurrentThreadPolicy(ThreadPolicy policy);
}  // namespace ysm
