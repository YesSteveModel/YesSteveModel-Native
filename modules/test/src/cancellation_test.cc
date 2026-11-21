#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "cancellation.h"
#include "java/opaque_ptr.h"

namespace ysm::test {
namespace {
TEST(CancellationTest, StartsUnrequestedAndRequestsOnlyItsOwnState) {
    CancellationState state;
    CancellationState independent;
    EXPECT_FALSE(state.IsRequested());
    state.Request();
    state.Request();
    EXPECT_TRUE(state.IsRequested());
    EXPECT_FALSE(independent.IsRequested());
}

TEST(CancellationTest, PublishesToMultipleIndependentConsumers) {
    CancellationState state;
    constexpr auto kConsumers = 8;
    std::latch ready{kConsumers};
    std::latch inspect{1};
    std::array<bool, kConsumers> observed{};
    std::vector<std::jthread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&, i] {
            ready.count_down();
            inspect.wait();
            observed[i] = state.IsRequested();
        });
    }
    ready.wait();
    state.Request();
    inspect.count_down();
    consumers.clear();
    for (int i = 0; i < kConsumers; ++i) {
        EXPECT_TRUE(observed[i]);
    }
}

TEST(CancellationTest, AcquireCheckPublishesRequestersPriorWrites) {
    CancellationState state;
    int payload = 0;
    int observed = 0;
    std::jthread consumer([&] {
        while (!state.IsRequested()) {
            std::this_thread::yield();
        }
        observed = payload;
    });
    payload = 73;
    state.Request();
    consumer.join();
    EXPECT_EQ(observed, 73);
}

TEST(CancellationTest, ConcurrentRequestsNeverResetState) {
    CancellationState state;
    CancellationState independent;
    std::latch start{1};
    std::atomic<bool> regressed{false};
    std::vector<std::jthread> requesters;
    for (int i = 0; i < 8; ++i) {
        requesters.emplace_back([&] {
            start.wait();
            for (int request = 0; request < 1000; ++request) {
                state.Request();
                if (!state.IsRequested()) {
                    regressed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    start.count_down();
    requesters.clear();
    EXPECT_FALSE(regressed.load());
    EXPECT_TRUE(state.IsRequested());
    EXPECT_FALSE(independent.IsRequested());
}

TEST(CancellationTest, RetainedConsumerOutlivesHandleAndReleasesLastBacking) {
    auto handle = java::MakeOpaquePtr<CancellationState>();
    auto acquired = java::AcquireOpaquePtr<CancellationState>(handle);
    ASSERT_TRUE(acquired.ok());
    std::weak_ptr<CancellationState> lifetime{*acquired};
    std::latch ready{1};
    std::latch inspect{1};
    bool observed = false;
    std::jthread consumer([state = *acquired, &ready, &inspect, &observed] {
        ready.count_down();
        inspect.wait();
        observed = state->IsRequested();
    });
    ready.wait();
    (*acquired)->Request();
    acquired->reset();
    java::DestroyOpaquePtr(handle);
    EXPECT_FALSE(lifetime.expired());
    inspect.count_down();
    consumer.join();
    EXPECT_TRUE(observed);
    EXPECT_TRUE(lifetime.expired());
}
}  // namespace
}  // namespace ysm::test
