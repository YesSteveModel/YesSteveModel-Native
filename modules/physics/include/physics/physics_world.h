#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <absl/status/statusor.h>

#include "physics/physics_asset.h"

namespace ysm::gfx::renderer {
struct BonePhysicsDelta;
}

namespace ysm::physics {

struct PhysicsPose {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
};

// Bone pivots are in model space, in meters and asset node order. The FK owner
// resolves scale validity, visibility and anchoring before passing active nodes.
struct PhysicsNodeTarget {
    PhysicsPose pivot;
    float scale = 1;
    bool active = true;
};

struct PhysicsFrame {
    std::array<double, 3> root_position{};
    std::array<float, 4> root_rotation{0, 0, 0, 1};
    std::span<const PhysicsNodeTarget> nodes;
};

enum class StepOutcome : uint8_t {
    kSuccess, kCapacityExceeded, kScratchExhausted, kNonFiniteState
};

struct StepResult {
    StepOutcome outcome = StepOutcome::kSuccess;
    uint32_t engine_error = 0;
    size_t peak_bytes = 0;
};

enum class ContactState : uint8_t {
    kNoSample, kIneligible, kNoStep, kNotSampled, kNoManifold, kSpeculative, kTouching
};

enum class DeactivationKind : uint16_t {
    kActive, kInvalidScale, kInvalidPose, kInvalidInertia, kCubesHidden, kChildrenHidden,
    kLostAnchor, kInactiveInput
};

struct DeactivationReason {
    DeactivationKind kind = DeactivationKind::kActive;
    // Sorted bone index, or the interrupted constraint index for kLostAnchor.
    uint16_t culprit = 0;
};
static_assert(sizeof(DeactivationReason) == 4);

struct DebugSnapshotOut {
    static constexpr size_t kNodeFloats = 8;
    static constexpr size_t kNodeInts = 3;
    static constexpr size_t kJointFloats = 33;
    static constexpr int32_t kSample = 1, kStepped = 2, kContactSampled = 4;

    struct Layout {
        size_t float_count;
        size_t int_count;
    };

    // Declaration order, independent of native solver slot order. See the JNI consumer.
    std::span<float> floats;
    std::span<int32_t> ints;
};

class PhysicsWorld {
public:
    static absl::StatusOr<PhysicsWorld> Create(std::shared_ptr<const PhysicsAsset> asset);
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
    ~PhysicsWorld();

    StepResult Reset(const PhysicsFrame& frame);
    StepResult Step(const PhysicsFrame& frame, float dt);

    // All three calls belong to the same thread. Only GatherBoneAttributes
    // runs with the JVM array pinned. The span uses the render bone preorder.
    bool PrepareGather() noexcept;
    void GatherBoneAttributes(std::span<const float> attributes) noexcept;
    StepResult AdvanceGathered(bool reset, float dt, const std::array<double, 3>& root_position,
                               const std::array<float, 4>& root_rotation) noexcept;
    std::span<const gfx::renderer::BonePhysicsDelta> ActiveDeltas() const noexcept;

    // Borrowed by the serial world owner after a successful Reset or Step.
    bool HasSample() const;
    bool IsNodeActive(size_t slot) const;
    PhysicsPose ReadNodePivot(size_t slot) const;
    const PhysicsNodeTarget& ReadAnimationTarget(size_t slot) const;
    // Relative animated attachment frames, in meters. An invalid FK endpoint has no reading.
    std::optional<PhysicsPose> ReadJointAnimationTarget(size_t joint) const;
    DeactivationReason ReadDeactivationReason(size_t slot) const;
    void SetDebugContactCapture(bool enabled);
    ContactState ReadContact(size_t a, size_t b) const;
    bool IsRotationSingular(size_t joint) const;
    DebugSnapshotOut::Layout DebugSnapshotLayout() const;
    // Called by the serial world owner. Buffers have the exact layout above and
    // are populated only when the returned flags include kSample.
    int32_t ReadDebugSnapshot(DebugSnapshotOut output) const;

private:
    friend struct PhysicsWorldTestAccess;
    class Impl;
    explicit PhysicsWorld(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace ysm::physics
