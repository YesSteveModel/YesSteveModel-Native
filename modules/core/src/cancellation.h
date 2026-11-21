#pragma once

#include <atomic>

namespace ysm {
// Shared cooperative cancellation only; the consumer owns its work and outcome.
class CancellationState final {
   public:
    void Request() noexcept { requested_.store(true, std::memory_order_release); }

    [[nodiscard]] bool IsRequested() const noexcept {
        return requested_.load(std::memory_order_acquire);
    }

   private:
    std::atomic<bool> requested_{false};
};
}  // namespace ysm
