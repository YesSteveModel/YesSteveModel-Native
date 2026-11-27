#include "native_joint.h"

#include <algorithm>
#include <cfloat>

#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

namespace ysm::physics {
namespace {
using Kind = schema::PhysicsConstraintKind;
using Limit = schema::LimitKind;

enum class NativeKind { kNone, kFixed, kHinge, kSixDof, kSixDofWithRotationLock };

bool FixedAxis(const Axis& axis) {
    return axis.limit == Limit::kLocked || (axis.limit == Limit::kHard && axis.lower == axis.upper);
}

NativeKind SelectNativeJoint(const Constraint& joint) {
    if (joint.kind == Kind::kFixed || (joint.kind == Kind::kHinge && FixedAxis(joint.axes[5]))) {
        return NativeKind::kFixed;
    }
    if (joint.kind == Kind::kHinge) {
        return NativeKind::kHinge;
    }
    if (joint.cone.limit == Limit::kFree &&
        std::all_of(joint.axes.begin() + 3, joint.axes.end(),
                    [](const Axis& axis) { return axis.limit == Limit::kLocked; })) {
        return NativeKind::kSixDofWithRotationLock;
    }
    const bool constrains_translation = joint.kind == Kind::kPoint ||
        std::any_of(joint.axes.begin(), joint.axes.begin() + 3,
                    [](const Axis& axis) { return axis.limit == Limit::kLocked || axis.limit == Limit::kHard; });
    return constrains_translation ? NativeKind::kSixDof : NativeKind::kNone;
}

bool FoldTranslation(const Constraint& joint) {
    return joint.kind == Kind::kGeneric &&
        std::all_of(joint.axes.begin(), joint.axes.begin() + 3, FixedAxis);
}

struct Limits {
    JPH::Vec3 lo = JPH::Vec3::sZero();
    JPH::Vec3 hi = JPH::Vec3::sZero();
};

Limits TranslationLimits(const Constraint& joint, float scale) {
    Limits limits;
    if (joint.kind == Kind::kGeneric && !FoldTranslation(joint)) {
        for (unsigned i = 0; i < 3; ++i) {
            const auto& axis = joint.axes[i];
            if (axis.limit == Limit::kHard) {
                limits.lo.SetComponent(i, axis.lower * scale);
                limits.hi.SetComponent(i, axis.upper * scale);
            } else if (axis.limit != Limit::kLocked) {
                limits.lo.SetComponent(i, -FLT_MAX);
                limits.hi.SetComponent(i, FLT_MAX);
            }
        }
    }
    return limits;
}
}  // namespace

bool HasNativeJoint(const Constraint& joint) {
    return SelectNativeJoint(joint) != NativeKind::kNone;
}

JPH::Vec3 NativeAttachmentA(const Constraint& joint, const RigidPose& frame, float scale) {
    if (!FoldTranslation(joint)) {
        return frame.position;
    }
    const auto value = [&](unsigned i) {
        return joint.axes[i].limit == Limit::kHard ? joint.axes[i].lower * scale : 0.0f;
    };
    return frame.position + frame.rotation * JPH::Vec3(value(0), value(1), value(2));
}

JPH::Ref<JPH::TwoBodyConstraint> MakeNativeJoint(
    const Constraint& joint, const std::array<RigidPose, 2>& frames,
    float scale, JPH::Body& a, JPH::Body& b) {
    const auto native = SelectNativeJoint(joint);
    if (native == NativeKind::kNone) {
        return nullptr;
    }
    const auto& f1 = frames[0];
    const auto& f2 = frames[1];
    constexpr auto space = JPH::EConstraintSpace::LocalToBodyCOM;
    const auto& hinge = joint.axes[5];
    if (native == NativeKind::kFixed) {
        const float angle = joint.kind == Kind::kHinge && hinge.limit == Limit::kHard ? hinge.lower : 0;
        const auto orientation = f1.rotation * JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), angle);
        JPH::FixedConstraintSettings settings;
        settings.mSpace = space;
        settings.mAutoDetectPoint = false;
        settings.mPoint1 = f1.position;
        settings.mPoint2 = f2.position;
        settings.mAxisX1 = orientation.RotateAxisX();
        settings.mAxisY1 = orientation.RotateAxisY();
        settings.mAxisX2 = f2.rotation.RotateAxisX();
        settings.mAxisY2 = f2.rotation.RotateAxisY();
        settings.mConstraintPriority = 1;
        return settings.Create(a, b);
    }
    if (native == NativeKind::kHinge) {
        JPH::HingeConstraintSettings settings;
        settings.mSpace = space;
        settings.mPoint1 = f1.position;
        settings.mPoint2 = f2.position;
        settings.mHingeAxis1 = f1.rotation.RotateAxisZ();
        settings.mHingeAxis2 = f2.rotation.RotateAxisZ();
        float bias = 0;
        if (hinge.limit == Limit::kHard) {
            bias = 0.5f * (hinge.lower + hinge.upper);
            const auto half = 0.5f * (hinge.upper - hinge.lower);
            settings.mLimitsMin = -half;
            settings.mLimitsMax = half;
        }
        settings.mNormalAxis1 =
            (f1.rotation * JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), bias)).RotateAxisX();
        settings.mNormalAxis2 = f2.rotation.RotateAxisX();
        settings.mConstraintPriority = 1;
        return settings.Create(a, b);
    }
    const auto [lo, hi] = TranslationLimits(joint, scale);
    JPH::SixDOFConstraintSettings settings;
    settings.mSpace = space;
    settings.mPosition1 = NativeAttachmentA(joint, f1, scale);
    settings.mPosition2 = f2.position;
    settings.mAxisX1 = f1.rotation.RotateAxisX();
    settings.mAxisY1 = f1.rotation.RotateAxisY();
    settings.mAxisX2 = f2.rotation.RotateAxisX();
    settings.mAxisY2 = f2.rotation.RotateAxisY();
    for (unsigned i = 0; i < 3; ++i) {
        settings.SetLimitedAxis(static_cast<JPH::SixDOFConstraintSettings::EAxis>(i), lo[i], hi[i]);
        if (native == NativeKind::kSixDofWithRotationLock) {
            settings.MakeFixedAxis(static_cast<JPH::SixDOFConstraintSettings::EAxis>(3 + i));
        }
    }
    settings.mConstraintPriority = 1;
    return settings.Create(a, b);
}

void RescaleNativeLimits(JPH::TwoBodyConstraint& native, const Constraint& joint, float scale) {
    if (joint.kind == Kind::kGeneric && native.GetSubType() == JPH::EConstraintSubType::SixDOF) {
        const auto [lo, hi] = TranslationLimits(joint, scale);
        static_cast<JPH::SixDOFConstraint&>(native).SetTranslationLimits(lo, hi);
    }
}

}  // namespace ysm::physics
