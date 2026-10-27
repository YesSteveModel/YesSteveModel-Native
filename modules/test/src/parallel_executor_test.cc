#include <array>
#include <atomic>
#include <cstddef>

#include <gtest/gtest.h>

#include "cpu_topology.h"
#include "renderer/parallel_executor.h"

namespace ysm::gfx::renderer::internal {
namespace {
TEST(ParallelExecutorPolicyTest, CreatesValidPlatformConfig) {
    EXPECT_GE(ParallelExecutor::Get().GetThreadCount(), 2);
    EXPECT_LE(ParallelExecutor::Get().GetThreadCount(), 8);
}

TEST(ParallelExecutorTest, TreatsRequestedSizeAsTotalWorkerCount) {
    std::array<std::atomic_bool, 4> initialized{};
    ParallelExecutor executor(4, [&](size_t index, size_t worker_count) {
        EXPECT_EQ(worker_count, 4);
        initialized[index].store(true, std::memory_order_relaxed);
    });
    EXPECT_EQ(executor.GetThreadCount(), 4);

    std::array<std::atomic_bool, 4> executed{};
    auto active_scope = executor.BeginActiveScope();
    executor.Execute([&](size_t index, size_t worker_count) {
        EXPECT_EQ(worker_count, 4);
        executed[index].store(true, std::memory_order_relaxed);
    });

    EXPECT_FALSE(initialized[0].load(std::memory_order_relaxed));
    EXPECT_TRUE(executed[0].load(std::memory_order_relaxed));
    for (size_t index = 1; index < initialized.size(); ++index) {
        EXPECT_TRUE(initialized[index].load(std::memory_order_relaxed));
        EXPECT_TRUE(executed[index].load(std::memory_order_relaxed));
    }
}

TEST(ParallelExecutorTest, ClampsZeroSizeToCallerWorker) {
    ParallelExecutor executor(0);
    EXPECT_EQ(executor.GetThreadCount(), 1);
}
}  // namespace
}  // namespace ysm::gfx::renderer::internal
