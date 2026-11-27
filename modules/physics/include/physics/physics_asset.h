#pragma once

#include <memory>

#include <absl/status/statusor.h>

#include "buffer.h"
#include "pimpl.h"

namespace ysm::physics {
class PhysicsAsset final {
public:
    ~PhysicsAsset();
    PhysicsAsset(const PhysicsAsset&) = delete;
    PhysicsAsset& operator=(const PhysicsAsset&) = delete;

private:
    friend class PhysicsWorld;
    friend class PhysicsAssetTest;
    friend absl::StatusOr<std::shared_ptr<const PhysicsAsset>> CompilePhysicsAsset(
        BufferViewR, BufferViewR);

    PhysicsAsset();
    YSM_PIMPL_DECLARE_ALIGN(Impl, 112, 16);
};

// Both payloads come from the same admitted player/main definition.
absl::StatusOr<std::shared_ptr<const PhysicsAsset>> CompilePhysicsAsset(
    BufferViewR geometry, BufferViewR physics);
}  // namespace ysm::physics
