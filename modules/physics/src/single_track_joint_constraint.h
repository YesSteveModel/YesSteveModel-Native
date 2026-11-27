#pragma once

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "physics_asset_internal.h"

#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

namespace ysm::physics {

class SingleTrackJointConstraint final : public JPH::TwoBodyConstraint {
public:
    static bool IsNeeded(const ysm::physics::Constraint& definition);

    // The world owns the immutable asset for the lifetime of its constraints.
    SingleTrackJointConstraint(JPH::Body& a, JPH::Body& b, const ysm::physics::Constraint& definition,
                               const std::array<RigidPose, 2>& frames_com, float a_scale);
    ~SingleTrackJointConstraint() override;
    void SetFrames(const std::array<RigidPose, 2>& frames_com, float a_scale);
    void SetAnimationTarget(const RigidPose& relative);
    static std::pair<JPH::Vec3, bool> ReadDebugAngles(JPH::QuatArg relative, schema::PhysicsConstraintKind kind);
    static bool IsRotationSingular(JPH::QuatArg relative, schema::PhysicsConstraintKind kind);
    bool IsRotationSingular() const;

    JPH::EConstraintSubType GetSubType() const override { return JPH::EConstraintSubType::User1; }
    JPH::Mat44 GetConstraintToBody1Matrix() const override;
    JPH::Mat44 GetConstraintToBody2Matrix() const override;
    JPH::Ref<JPH::ConstraintSettings> GetConstraintSettings() const override;
    void NotifyShapeChanged(const JPH::BodyID& body, JPH::Vec3Arg delta_com) override;
    void ResetWarmStart() override;
    void SetupVelocityConstraint(float dt) override;
    void WarmStartVelocityConstraint(float ratio) override;
    bool SolveVelocityConstraint(float dt) override;
    bool SolvePositionConstraint(float dt, float baumgarte) override;
    void SaveState(JPH::StateRecorder& stream) const override;
    void RestoreState(JPH::StateRecorder& stream) override;
#ifdef JPH_DEBUG_RENDERER
    void DrawConstraint(JPH::DebugRenderer*) const override {}
#endif

private:
    struct Row;
    struct Geometry;
    Geometry Measure(bool include_linear) const;

    const ysm::physics::Constraint& definition_;
    std::array<RigidPose, 2> frames_;
    float a_scale_;
    std::vector<Row> rows_;
    size_t first_hard_ = 0;
    bool has_linear_ = false;
    bool has_rotation_axes_ = false;
    bool has_cone_ = false;
    bool has_animation_rotation_ = false;
    bool rest_singular_ = false;
    bool animation_singular_ = false;
    JPH::Vec3 rest_angles_ = JPH::Vec3::sZero();
    JPH::Vec3 animation_angles_ = JPH::Vec3::sZero();
    JPH::Vec3 animation_position_ = JPH::Vec3::sZero();
    JPH::Vec3 lever_a_ = JPH::Vec3::sZero();
    JPH::Vec3 lever_b_ = JPH::Vec3::sZero();
};

}  // namespace ysm::physics
