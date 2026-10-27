#include <gtest/gtest.h>

#include <array>
#include <thread>
#include <vector>

#include "thread_scheduling.h"
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

TEST(ThreadSchedulingTest, NormalAndInvalidPolicyDoNotTouchOsState) {
    const auto normal = ApplyCurrentThreadPolicy(ThreadPolicy::kNormal);
    EXPECT_EQ(normal.outcome, Outcome::kUnchanged);
    EXPECT_EQ(normal.step_count, 0);
    EXPECT_EQ(normal.observed_mask, 0);
    const auto invalid = ApplyCurrentThreadPolicy(static_cast<ThreadPolicy>(-1));
    EXPECT_EQ(invalid.outcome, Outcome::kFailed);
    EXPECT_EQ(invalid.step_count, 0);
    EXPECT_EQ(invalid.observed_mask, 0);
}

TEST(ThreadSchedulingTest, ReportEncodingMatchesJniAbi) {
    ThreadPolicyReport report;
    report.outcome = Outcome::kPartial;
    report.platform = ThreadPolicyPlatform::kWindows;
    report.observed.fill(99);
    report.observed_mask = 1;
    report.step_count = 2;
    report.steps[0] = {Operation::kSetMemory, Result::kFailed, Domain::kWin32, 0xffffffffULL};
    report.steps[1] = {Operation::kQueryAfter, Result::kOk, Domain::kNone, 0};
    const std::vector<int64_t> expected = {
        3, 1, 1, 99, 0, 0, 0, 0, 2,
        4, 3, 1, 4294967295LL,
        8, 0, 0, 0,
    };
    const auto encoded = report.Encode();
    ASSERT_EQ(report.EncodedSize(), expected.size());
    EXPECT_EQ(std::vector(encoded.begin(), encoded.begin() + report.EncodedSize()), expected);
}

TEST(ThreadSchedulingTest, PreservedLowerQosIsDegradedUntilExactRequestIsReadBack) {
    using thread_scheduling_internal::ClassifyQosReadback;
    constexpr int kUtility = 0x11;
    constexpr int kBackground = 0x09;
    EXPECT_EQ(ClassifyQosReadback(kUtility, kUtility, 0), Outcome::kApplied);
    EXPECT_EQ(ClassifyQosReadback(kUtility, kUtility, -1), Outcome::kDegraded);
    EXPECT_EQ(ClassifyQosReadback(kUtility, kBackground, 0), Outcome::kDegraded);
    EXPECT_EQ(ClassifyQosReadback(kBackground, kBackground, -15), Outcome::kDegraded);
    EXPECT_EQ(ClassifyQosReadback(kBackground, kUtility, 0), Outcome::kFailed);
    EXPECT_EQ(ClassifyQosReadback(kUtility, 0, 0), Outcome::kFailed);
}

TEST(ThreadSchedulingTest, DedicatedNativeThreadsApplyAndReadBackEachPolicy) {
    for (const auto policy : {ThreadPolicy::kBelowNormal, ThreadPolicy::kLow}) {
        ThreadPolicyReport report;
        std::thread worker([&] { report = ApplyCurrentThreadPolicy(policy); });
        worker.join();
        EXPECT_EQ(report.outcome, Outcome::kApplied);
#if YSM_WINDOWS
        EXPECT_EQ(report.platform, ThreadPolicyPlatform::kWindows);
        EXPECT_NE(report.observed_mask & 1, 0);
        if (policy == ThreadPolicy::kBelowNormal) {
            EXPECT_LE(report.observed[0], THREAD_PRIORITY_BELOW_NORMAL);
        } else {
            EXPECT_NE(report.observed_mask & 4, 0);
            EXPECT_EQ(report.observed[2], 5);
        }
#elif YSM_LINUX || YSM_ANDROID
        EXPECT_EQ(report.observed_mask, 3);
        EXPECT_GE(report.observed[0], policy == ThreadPolicy::kLow ? 10 : 5);
        if (policy == ThreadPolicy::kLow) EXPECT_EQ(report.observed[1], SCHED_BATCH);
#elif YSM_MACOS
        EXPECT_EQ(report.observed_mask, 24);
        EXPECT_EQ(report.observed[3], policy == ThreadPolicy::kLow
                                          ? QOS_CLASS_BACKGROUND : QOS_CLASS_UTILITY);
        EXPECT_EQ(report.observed[4], 0);
#endif
    }
}

#if YSM_WINDOWS
using thread_scheduling_internal::WindowsCall;

struct FakeWindows {
    int cpu = 0;
    int memory = 5;
    int saved_cpu = 0;
    int saved_memory = 5;
    bool background = false;
    bool memory_api = true;
    int cpu_queries = 0;
    int after_cpu_offset = 0;
    uint32_t before_error = 0;
    uint32_t after_cpu_error = 0;
    uint32_t after_memory_error = 0;
    std::array<uint32_t, 9> errors{};
    std::vector<Operation> calls;

    WindowsCall QueryCpu() {
        const auto before = cpu_queries++ == 0;
        const auto error = before ? before_error : after_cpu_error;
        return error ? WindowsCall{false, 0, error}
                     : WindowsCall{true, cpu + (before ? 0 : after_cpu_offset)};
    }

    WindowsCall SetPriority(int value) {
        const auto operation = value == THREAD_MODE_BACKGROUND_BEGIN ? Operation::kBeginBackground
                               : value == THREAD_MODE_BACKGROUND_END ? Operation::kEndBackground
                                                                      : Operation::kSetCpu;
        calls.push_back(operation);
        const auto error = errors[static_cast<size_t>(operation)];
        if (error) return {false, 0, error};
        if (operation == Operation::kBeginBackground) {
            if (background) return {false, 0, ERROR_THREAD_MODE_ALREADY_BACKGROUND};
            background = true;
            saved_cpu = cpu;
            saved_memory = memory;
            cpu = THREAD_PRIORITY_IDLE;
            memory = 1;
        } else if (operation == Operation::kEndBackground) {
            background = false;
            cpu = saved_cpu;
            memory = saved_memory;
        } else {
            cpu = value;
        }
        return {true};
    }

    ThreadPolicyReport Apply(ThreadPolicy policy = ThreadPolicy::kLow) {
        cpu_queries = 0;
        auto query_cpu = [&] { return QueryCpu(); };
        auto query_memory = [&] {
            return after_memory_error ? WindowsCall{false, 0, after_memory_error}
                                      : WindowsCall{true, memory};
        };
        auto set_priority = [&](int value) { return SetPriority(value); };
        auto set_memory = [&] {
            calls.push_back(Operation::kSetMemory);
            const auto error = errors[static_cast<size_t>(Operation::kSetMemory)];
            if (error) return WindowsCall{false, 0, error};
            memory = 5;
            return WindowsCall{true};
        };
        return thread_scheduling_internal::ApplyWindowsThreadPolicy(
            policy, {memory_api, query_cpu, query_memory, set_priority, set_memory});
    }
};

bool HasStep(const ThreadPolicyReport& report, Operation operation, Result result,
             int64_t error = 0) {
    const auto domain = result == Result::kFailed ? Domain::kWin32 : Domain::kNone;
    for (size_t i = 0; i < report.step_count; ++i) {
        const auto& step = report.steps[i];
        if (step.operation == operation && step.result == result &&
            step.error_domain == domain && step.native_error == error) return true;
    }
    return false;
}

TEST(ThreadSchedulingWindowsTest, BaselineFailureDoesNotBlindlyMutate) {
    FakeWindows os;
    os.before_error = ERROR_ACCESS_DENIED;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kFailed);
    EXPECT_TRUE(os.calls.empty());
    EXPECT_EQ(report.observed_mask, 0);
    EXPECT_TRUE(HasStep(report, Operation::kQueryBefore, Result::kFailed, ERROR_ACCESS_DENIED));
}

TEST(ThreadSchedulingWindowsTest, BelowNormalCpuFailureAndReadbackFailureAreDistinct) {
    FakeWindows os;
    os.errors[2] = ERROR_ACCESS_DENIED;
    auto report = os.Apply(ThreadPolicy::kBelowNormal);
    EXPECT_EQ(report.outcome, Outcome::kFailed);
    EXPECT_TRUE(HasStep(report, Operation::kSetCpu, Result::kFailed, ERROR_ACCESS_DENIED));
    os.errors[2] = 0;
    os.after_cpu_error = ERROR_INVALID_HANDLE;
    report = os.Apply(ThreadPolicy::kBelowNormal);
    EXPECT_EQ(report.outcome, Outcome::kPartial);
    EXPECT_EQ(report.observed_mask, 0);
}

TEST(ThreadSchedulingWindowsTest, BothPoliciesPreserveHostLowerCpuAndMemory) {
    for (const auto policy : {ThreadPolicy::kBelowNormal, ThreadPolicy::kLow}) {
        for (const auto memory_api : {false, true}) {
            SCOPED_TRACE(static_cast<int>(policy));
            SCOPED_TRACE(memory_api);
            FakeWindows os;
            os.cpu = THREAD_PRIORITY_IDLE;
            os.memory = 2;
            os.memory_api = memory_api;
            const auto report = os.Apply(policy);
            EXPECT_EQ(report.outcome, policy == ThreadPolicy::kLow
                                          ? Outcome::kDegraded : Outcome::kApplied);
            EXPECT_TRUE(os.calls.empty());
            EXPECT_EQ(report.observed[0], THREAD_PRIORITY_IDLE);
            EXPECT_EQ(os.memory, 2);
            EXPECT_TRUE(HasStep(report, Operation::kSetCpu, Result::kSkipped));
            if (policy == ThreadPolicy::kLow) {
                EXPECT_EQ(report.observed_mask, memory_api ? 5 : 1);
                if (memory_api) EXPECT_EQ(report.observed[2], 2);
                EXPECT_TRUE(HasStep(report, Operation::kBeginBackground, Result::kSkipped));
            }
        }
    }
}

TEST(ThreadSchedulingWindowsTest, SuccessfulBeginCannotHideAnIncreasedCpuReadback) {
    FakeWindows os;
    os.after_cpu_offset = 16;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kPartial);
    EXPECT_EQ(report.observed[0], THREAD_PRIORITY_ABOVE_NORMAL);
    EXPECT_TRUE(HasStep(report, Operation::kBeginBackground, Result::kOk));
    EXPECT_EQ(report.observed[2], 5);
}

TEST(ThreadSchedulingWindowsTest, MissingOptionalApisUseCpuLowest) {
    FakeWindows os;
    os.memory_api = false;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kDegraded);
    EXPECT_EQ(os.cpu, THREAD_PRIORITY_LOWEST);
    EXPECT_EQ(os.calls, (std::vector{Operation::kSetCpu}));
    EXPECT_TRUE(HasStep(report, Operation::kSetMemory, Result::kUnsupported));
}

TEST(ThreadSchedulingWindowsTest, BeginFailureRetainsCauseAndUsesFallback) {
    FakeWindows os;
    os.errors[3] = 0xffffffffU;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kDegraded);
    EXPECT_EQ(os.calls, (std::vector{Operation::kBeginBackground, Operation::kSetCpu}));
    EXPECT_TRUE(HasStep(report, Operation::kBeginBackground, Result::kFailed, 4294967295LL));
    EXPECT_EQ(os.cpu, THREAD_PRIORITY_LOWEST);
}

TEST(ThreadSchedulingWindowsTest, MemoryFailureRollsBackOnlyOwnedBackgroundThenFallsBack) {
    FakeWindows os;
    os.errors[4] = ERROR_ACCESS_DENIED;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kDegraded);
    EXPECT_EQ(os.calls, (std::vector{Operation::kBeginBackground, Operation::kSetMemory,
                                   Operation::kEndBackground, Operation::kSetCpu}));
    EXPECT_FALSE(os.background);
    EXPECT_EQ(os.cpu, THREAD_PRIORITY_LOWEST);
    EXPECT_EQ(os.memory, 5);
    EXPECT_TRUE(HasStep(report, Operation::kSetMemory, Result::kFailed, ERROR_ACCESS_DENIED));
}

TEST(ThreadSchedulingWindowsTest, FailedRollbackIsPartialAndNeverClaimsCpuFallback) {
    FakeWindows os;
    os.errors[4] = ERROR_ACCESS_DENIED;
    os.errors[5] = ERROR_INVALID_FUNCTION;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kPartial);
    EXPECT_TRUE(os.background);
    EXPECT_EQ(os.memory, 1);
    EXPECT_TRUE(HasStep(report, Operation::kSetMemory, Result::kFailed, ERROR_ACCESS_DENIED));
    EXPECT_TRUE(HasStep(report, Operation::kEndBackground, Result::kFailed, ERROR_INVALID_FUNCTION));
    EXPECT_FALSE(HasStep(report, Operation::kSetCpu, Result::kOk));
}

TEST(ThreadSchedulingWindowsTest, FallbackFailureRetainsBothOriginalAndFallbackErrors) {
    FakeWindows os;
    os.errors[4] = ERROR_ACCESS_DENIED;
    os.errors[2] = ERROR_INVALID_HANDLE;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kFailed);
    EXPECT_FALSE(os.background);
    EXPECT_TRUE(HasStep(report, Operation::kSetMemory, Result::kFailed, ERROR_ACCESS_DENIED));
    EXPECT_TRUE(HasStep(report, Operation::kSetCpu, Result::kFailed, ERROR_INVALID_HANDLE));
}

TEST(ThreadSchedulingWindowsTest, EveryReadbackFailurePreventsApplied) {
    for (const auto memory : {false, true}) {
        FakeWindows os;
        (memory ? os.after_memory_error : os.after_cpu_error) = ERROR_ACCESS_DENIED;
        const auto report = os.Apply();
        EXPECT_EQ(report.outcome, Outcome::kPartial);
        EXPECT_EQ(report.observed_mask, memory ? 1 : 4);
        EXPECT_TRUE(HasStep(report, Operation::kQueryAfter, Result::kFailed, ERROR_ACCESS_DENIED));
    }
}

TEST(ThreadSchedulingWindowsTest, AlreadyBackgroundFailureNeverGrantsRollbackOwnership) {
    FakeWindows os;
    os.background = true;
    os.cpu = THREAD_PRIORITY_LOWEST;
    const auto report = os.Apply();
    EXPECT_EQ(report.outcome, Outcome::kDegraded);
    EXPECT_TRUE(os.background);
    EXPECT_EQ(os.calls, (std::vector{Operation::kBeginBackground}));
    EXPECT_TRUE(HasStep(report, Operation::kBeginBackground, Result::kFailed,
                        ERROR_THREAD_MODE_ALREADY_BACKGROUND));
}

TEST(ThreadSchedulingWindowsTest, RealHostIdleAndRepeatedLowPreserveThreadOwnership) {
    const auto parent_cpu = GetThreadPriority(GetCurrentThread());
    std::thread worker([] {
        ASSERT_TRUE(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE));
        auto report = ApplyCurrentThreadPolicy(ThreadPolicy::kBelowNormal);
        EXPECT_EQ(report.outcome, Outcome::kApplied);
        EXPECT_EQ(report.observed[0], THREAD_PRIORITY_IDLE);
        report = ApplyCurrentThreadPolicy(ThreadPolicy::kLow);
        EXPECT_EQ(report.outcome, Outcome::kDegraded);
        EXPECT_EQ(report.observed[0], THREAD_PRIORITY_IDLE);
        EXPECT_TRUE(HasStep(report, Operation::kBeginBackground, Result::kSkipped));
        report = ApplyCurrentThreadPolicy(ThreadPolicy::kLow);
        EXPECT_EQ(report.outcome, Outcome::kDegraded);
        EXPECT_EQ(report.observed[0], THREAD_PRIORITY_IDLE);
        EXPECT_FALSE(HasStep(report, Operation::kEndBackground, Result::kOk));
    });
    worker.join();
    EXPECT_EQ(GetThreadPriority(GetCurrentThread()), parent_cpu);
}

TEST(ThreadSchedulingWindowsTest, RealRepeatedLowPreservesBackgroundCpuAndNormalMemory) {
    std::thread worker([] {
        const auto first = ApplyCurrentThreadPolicy(ThreadPolicy::kLow);
        ASSERT_EQ(first.outcome, Outcome::kApplied);
        const auto repeated = ApplyCurrentThreadPolicy(ThreadPolicy::kLow);
        EXPECT_EQ(repeated.outcome, Outcome::kDegraded);
        EXPECT_LE(repeated.observed[0], first.observed[0]);
        EXPECT_EQ(repeated.observed[2], 5);
        EXPECT_FALSE(HasStep(repeated, Operation::kEndBackground, Result::kOk));
    });
    worker.join();
}
#endif
}  // namespace
}  // namespace ysm
