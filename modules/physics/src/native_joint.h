#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

#include "physics_asset_internal.h"

namespace ysm::physics {

bool HasNativeJoint(const Constraint& joint);

// The PointConstraintPart shortcut for three fixed translations ignores c_A
// in its velocity solve. Move that equality into the A attachment for both solves.
JPH::Vec3 NativeAttachmentA(const Constraint& joint, const RigidPose& frame, float scale);
JPH::Ref<JPH::TwoBodyConstraint> MakeNativeJoint(
    const Constraint& joint, const std::array<RigidPose, 2>& frames,
    float scale, JPH::Body& a, JPH::Body& b);
void RescaleNativeLimits(JPH::TwoBodyConstraint& native, const Constraint& joint, float scale);

}  // namespace ysm::physics
