#include "../src/native_joint.h"
#include "../src/single_track_joint_constraint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <gtest/gtest.h>

#include "physics/runtime.h"

namespace ysm::physics {
namespace {
using JPH::Quat;
using JPH::Vec3;
using Kind = schema::PhysicsConstraintKind;
using Limit = schema::LimitKind;
using Target = schema::SpringTarget;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDt = 1.0f / 60.0f;
constexpr float kContactSlop = 0.004f;
constexpr float Rad(float degrees) { return degrees * (kPi / 180); }
constexpr float Deg(float radians) { return radians * (180 / kPi); }
Quat Turn(Vec3 axis, float degrees) { return Quat::sRotation(axis, Rad(degrees)); }
RigidPose At(Vec3 position) { return {.position = position}; }
RigidPose At(Quat rotation) { return {.rotation = rotation}; }

JPH::SpringSettings Frequency(float hz = 1, float ratio = 1) {
    return {JPH::ESpringMode::FrequencyAndDamping, hz, ratio};
}

JPH::SpringSettings Coefficients(float k, float c) {
    return {JPH::ESpringMode::StiffnessAndDamping, k, c};
}

Constraint Joint(Kind kind = Kind::kGeneric) { return {.kind = kind}; }
Axis Hard(float lo, float hi) { return {.limit = Limit::kHard, .lower = lo, .upper = hi}; }
Axis Soft(float lo, float hi, JPH::SpringSettings spring) {
    return {.limit = Limit::kSoft, .lower = lo, .upper = hi, .soft_spring = spring};
}
void Drive(Axis& axis, Target target = Target::kAnimation, JPH::SpringSettings spring = Frequency()) {
    axis.target = target;
    axis.target_spring = spring;
}

class TestLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return JPH::BroadPhaseLayer(0); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "test"; }
#endif
};

class TestPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a != b; }
};

struct Runtime {
    Runtime() { InitializeRuntime(); }
};

struct Setup {
    RigidPose b;
    RigidPose a;
    float mass = 1;
    float scale = 1;
    bool swap = false;
    bool dynamic_a = false;
};

// Physical B stays dynamic even in a reversed joint so both arrangements see the same load.
class Rig : private Runtime {
public:
    explicit Rig(Constraint definition, Setup setup = {}) : definition_(std::move(definition)) {
        world_.Init(8, 1, 16, 16, layers_, broad_filter_, pair_filter_);
        world_.SetGravity(Vec3::sZero());
        auto& bodies = Bodies();
        const JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(std::sqrt(2.5f));
        auto make_body = [&](RigidPose pose, JPH::EMotionType motion) {
            JPH::BodyCreationSettings settings(sphere.GetPtr(), pose.position, pose.rotation, motion, 0);
            settings.mAllowSleeping = false;
            settings.mLinearDamping = settings.mAngularDamping = 0;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = setup.mass;
            auto* body = bodies.CreateBody(settings);
            bodies.AddBody(body->GetID(), motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate
                                                                           : JPH::EActivation::Activate);
            return body;
        };
        auto* a = make_body(setup.a, setup.dynamic_a ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static);
        auto* b = make_body(setup.b, JPH::EMotionType::Dynamic);
        id_a = a->GetID();
        id_b = b->GetID();
        if (setup.swap) {
            std::swap(a, b);
        }
        native_ = MakeNativeJoint(definition_, definition_.frames, setup.scale, *a, *b);
        if (native_) {
            world_.AddConstraint(native_);
        }
        if (SingleTrackJointConstraint::IsNeeded(definition_)) {
            track = new SingleTrackJointConstraint(*a, *b, definition_, definition_.frames, setup.scale);
            world_.AddConstraint(track);
        }
    }

    ~Rig() {
        if (track) {
            world_.RemoveConstraint(track);
            track = nullptr;
        }
        if (native_) {
            world_.RemoveConstraint(native_);
            native_ = nullptr;
        }
        for (auto id : {id_a, id_b}) {
            Bodies().RemoveBody(id);
            Bodies().DestroyBody(id);
        }
        if (!wall_.IsInvalid()) {
            Bodies().RemoveBody(wall_);
            Bodies().DestroyBody(wall_);
        }
    }

    JPH::BodyInterface& Bodies() { return world_.GetBodyInterface(); }
    Quat Rotation() { return Bodies().GetRotation(id_b); }
    Vec3 Position() { return Vec3(Bodies().GetCenterOfMassPosition(id_b)); }
    Vec3 Angular() { return Bodies().GetAngularVelocity(id_b); }
    Vec3 Linear() { return Bodies().GetLinearVelocity(id_b); }
    void Velocity(Vec3 v, Vec3 w) { Bodies().SetLinearAndAngularVelocity(id_b, v, w); }
    void TargetPose(RigidPose target) { track->SetAnimationTarget(target); }
    void SetGravity(Vec3 gravity) { world_.SetGravity(gravity); }
    void AddWall(float face_x) {
        auto physics_settings = world_.GetPhysicsSettings();
        physics_settings.mPenetrationSlop = kContactSlop;
        world_.SetPhysicsSettings(physics_settings);
        const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(Vec3(0.25f, 3, 3));
        JPH::BodyCreationSettings settings(shape.GetPtr(), Vec3(face_x + 0.25f, 0, 0), Quat::sIdentity(),
                                           JPH::EMotionType::Static, 1);
        wall_ = Bodies().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    }
    bool ContactWithWall() const { return world_.WereBodiesInContact(id_b, wall_); }
    void Step(int count = 1, float dt = kDt, int substeps = 1) {
        for (int i = 0; i < count; ++i) {
            ASSERT_EQ(world_.Update(dt, substeps, &scratch_, &jobs_), JPH::EPhysicsUpdateError::None);
        }
    }

    JPH::BodyID id_a, id_b;
    JPH::Ref<SingleTrackJointConstraint> track;
private:
    TestLayers layers_;
    TestPairs pair_filter_;
    JPH::ObjectVsBroadPhaseLayerFilter broad_filter_;
    JPH::PhysicsSystem world_;
    JPH::JobSystemSingleThreaded jobs_{64};
    JPH::TempAllocatorMalloc scratch_;
    Constraint definition_;
    JPH::Ref<JPH::TwoBodyConstraint> native_;
    JPH::BodyID wall_;
};

float ConeAngle(Quat relative) {
    const Vec3 x = relative.RotateAxisX();
    return Deg(std::atan2(std::hypot(x.GetY(), x.GetZ()), x.GetX()));
}

float TwistDegrees(Quat relative, Vec3 axis) {
    return Deg(std::remainder(2 * std::atan2(relative.GetXYZ().Dot(axis), relative.GetW()), 2 * kPi));
}

void ExpectVector(Vec3 actual, Vec3 expected, float tolerance = 1.0e-5f) {
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_NEAR(actual[i], expected[i], tolerance);
    }
}

TEST(SingleTrackJointTest, FullAngularLimitsAddNoRowsAndLeaveMotorResponseIntact) {
    const std::array basis{Vec3::sAxisX(), Vec3::sAxisY(), Vec3::sAxisZ()};
    for (unsigned i = 0; i < basis.size(); ++i) {
        const unsigned slot = 3 + i;
        SCOPED_TRACE(slot);
        const auto initial = At(Turn(basis[i], 120));
        auto joint = Joint();
        joint.axes[slot] = Hard(-kPi, kPi);
        EXPECT_FALSE(SingleTrackJointConstraint::IsNeeded(joint));
        for (const auto spring : {Frequency(2), Coefficients(12, 1)}) {
            joint.axes[slot] = Soft(-kPi, kPi, spring);
            EXPECT_FALSE(SingleTrackJointConstraint::IsNeeded(joint));

            auto bounded = joint;
            bounded.axes[slot] = Soft(-kPi / 2, kPi / 2, spring);
            Rig limited(bounded, {.b = initial});
            ASSERT_TRUE(limited.track);
            limited.Step();
            EXPECT_LT(limited.Angular().Dot(basis[i]), -0.01f);

            auto with_motor = joint;
            Drive(with_motor.axes[slot], Target::kRestPose, spring);
            auto motor_only = Joint();
            Drive(motor_only.axes[slot], Target::kRestPose, spring);
            Rig combined(with_motor, {.b = initial}), driven(motor_only, {.b = initial});
            ASSERT_TRUE(combined.track);
            ASSERT_TRUE(driven.track);
            combined.Step();
            driven.Step();
            EXPECT_LT(combined.Angular().Dot(basis[i]), -0.01f);
            ExpectVector(combined.Angular(), driven.Angular());
        }
    }
}

TEST(SingleTrackJointTest, SingleTwistLockProjectsAngularVelocityOntoTheFreePlane) {
    auto joint = Joint();
    joint.axes[3].limit = Limit::kLocked;
    Rig rig(joint, {.b = At(Turn(Vec3::sAxisZ(), 90))});
    rig.Velocity(Vec3::sZero(), Vec3::sAxisX());
    rig.Step();
    ExpectVector(rig.Angular(), Vec3(0.5f, -0.5f, 0));

    joint.axes[3].limit = Limit::kFree;
    joint.axes[4].limit = joint.axes[5].limit = Limit::kLocked;
    Rig double_lock(joint);
    double_lock.Velocity(Vec3::sZero(), Vec3(0.6f, -0.8f, 1.2f));
    double_lock.Step();
    ExpectVector(double_lock.Angular(), Vec3(0.6f, 0, 0));
}

TEST(SingleTrackJointTest, SwingSpringUsesFrameAEvenAfterNinetyDegreesOfTwist) {
    auto joint = Joint();
    Drive(joint.axes[4]);
    const auto swing = Turn(Vec3::sAxisY(), 20);
    const auto twist = Turn(Vec3::sAxisX(), 90);
    Rig pure(joint, {.b = At(swing)});
    Rig twisted(joint, {.b = At(swing * twist)});
    twisted.TargetPose(At(twist));
    pure.Step();
    twisted.Step();
    EXPECT_LT(pure.Angular().GetY(), -0.1f);
    ExpectVector(twisted.Angular(), pure.Angular());

    joint.axes[3].limit = Limit::kLocked;
    joint.axes[5] = Soft(Rad(-20), Rad(20), Coefficients(10, 0));
    Rig lock_and_soft(joint, {.b = At(Turn(Vec3::sAxisZ(), 40))});
    lock_and_soft.Velocity(Vec3::sZero(), Vec3::sAxisX());
    lock_and_soft.Step();
    const Vec3 omega = lock_and_soft.Angular();
    EXPECT_NEAR(omega.GetX() + std::tan(Rad(20)) * omega.GetY(), 0, 1.0e-5f);
    EXPECT_LT(omega.GetZ(), 0);
}

TEST(SingleTrackJointTest, ObliqueSwingLocksProjectOntoTheGradientOfTheMeasuredCoordinate) {
    const Quat pose = Turn(Vec3::sAxisZ(), -31) * Turn(Vec3::sAxisY(), 37) * Turn(Vec3::sAxisX(), 83);
    const Vec3 incoming(0.8f, -0.3f, 1.1f);
    for (unsigned component : {1u, 2u}) {
        // Recover the swing log directly from the direction of B's X axis. A central difference
        // supplies an independent Jacobian for the sphere's analytical velocity projection.
        auto measure = [&](Quat q) {
            const auto x = q.RotateAxisX();
            const auto normal = Vec3::sAxisX().Cross(x);
            return std::atan2(normal.Length(), x.GetX()) * normal[component] / normal.Length();
        };
        Vec3 gradient = Vec3::sZero();
        const std::array axes{Vec3::sAxisX(), Vec3::sAxisY(), Vec3::sAxisZ()};
        for (unsigned i = 0; i < 3; ++i) {
            constexpr float delta = 0.001f;
            const float upper = measure(Quat::sRotation(axes[i], delta) * pose);
            const float lower = measure(Quat::sRotation(axes[i], -delta) * pose);
            gradient.SetComponent(i, (upper - lower) / (2 * delta));
        }
        gradient = gradient.Normalized();
        auto joint = Joint();
        joint.axes[3 + component].limit = Limit::kLocked;
        Rig rig(joint, {.b = At(pose)});
        rig.Velocity(Vec3::sZero(), incoming);
        rig.Step();
        ExpectVector(rig.Angular(), incoming - gradient.Dot(incoming) * gradient, 0.00025f);
    }
}

TEST(SingleTrackJointTest, HalfTurnAndShortestArcIgnoreQuaternionSignAndTheFreeAxisCut) {
    for (Kind kind : {Kind::kHinge, Kind::kGeneric}) {
        const bool hinge = kind == Kind::kHinge;
        const Vec3 axis = hinge ? Vec3::sAxisZ() : Vec3::sAxisX();
        const unsigned slot = hinge ? 5 : 3;
        auto joint = Joint(kind);
        Drive(joint.axes[slot]);
        const Quat half(axis.GetX(), axis.GetY(), axis.GetZ(), 0);
        for (const Quat goal : {half, -half, Turn(axis, 180), Turn(axis, -180)}) {
            Rig rig(joint);
            rig.TargetPose(At(goal));
            rig.Step();
            EXPECT_GT(rig.Angular().Dot(axis), 0);
        }
        for (const Axis limit : {Axis{}, Hard(-kPi, kPi), Soft(Rad(-170), Rad(170), Coefficients(0, 0))}) {
            joint.axes[slot] = limit;
            Drive(joint.axes[slot]);
            Rig rig(joint, {.b = At(Turn(axis, 179))});
            rig.TargetPose(At(Turn(axis, -179)));
            rig.Step();
            EXPECT_GT(rig.Angular().Dot(axis), 0);
            EXPECT_LT(rig.Angular().Dot(axis), 0.1f);
        }
    }
}

TEST(SingleTrackJointTest, BoundedMotorTakesTheAllowedLongArcAndKeepsPreloadAtTheStop) {
    for (Kind kind : {Kind::kHinge, Kind::kGeneric}) {
        SCOPED_TRACE(static_cast<int>(kind));
        const bool hinge = kind == Kind::kHinge;
        const unsigned slot = hinge ? 5 : 3;
        const Vec3 axis = hinge ? Vec3::sAxisZ() : Vec3::sAxisX();
        auto joint = Joint(kind);
        joint.axes[slot] = Hard(Rad(-170), Rad(170));
        Drive(joint.axes[slot]);
        Rig long_arc(joint, {.b = At(Turn(axis, 169))});
        long_arc.TargetPose(At(Turn(axis, -169)));
        float previous = 169, travelled = 0;
        for (int i = 0; i < 180; ++i) {
            long_arc.Step();
            const float current = TwistDegrees(long_arc.Rotation(), axis);
            travelled += std::remainder(current - previous, 360.0f);
            previous = current;
        }
        EXPECT_NEAR(travelled, -338, 0.1f);

        joint.axes[slot] = Hard(Rad(-30), Rad(30));
        Drive(joint.axes[slot], Target::kAnimation, Frequency(6, 0.7f));
        Rig stop(joint, {.b = At(Turn(axis, 30))});
        stop.TargetPose(At(Turn(axis, 45)));
        stop.Step(40);
        for (int i = 0; i < 45; ++i) {
            stop.Bodies().AddTorque(stop.id_b, -20 * axis);
            stop.Step();
            EXPECT_NEAR(TwistDegrees(stop.Rotation(), axis), 30, 0.5f);
        }
        stop.TargetPose(At(Turn(axis, 5)));
        stop.Step(60);
        EXPECT_NEAR(TwistDegrees(stop.Rotation(), axis), 5, 0.1f);
    }
}

TEST(SingleTrackJointTest, PeriodicSoftBoundsUseTheMidpointAtBothSidesOfThePrincipalCut) {
    for (Kind kind : {Kind::kHinge, Kind::kGeneric}) {
        const bool hinge = kind == Kind::kHinge;
        const auto axis = hinge ? Vec3::sAxisZ() : Vec3::sAxisX();
        const unsigned slot = hinge ? 5 : 3;
        auto rate = [&](float angle, float speed, float lo, float hi, JPH::SpringSettings spring) {
            auto joint = Joint(kind);
            joint.axes[slot] = Soft(Rad(lo), Rad(hi), spring);
            Rig rig(joint, {.b = At(Turn(axis, angle))});
            rig.Velocity(Vec3::sZero(), speed * axis);
            rig.Step();
            return rig.Angular().Dot(axis);
        };
        const auto elastic = Coefficients(30, 0);
        const float clockwise = rate(179, 0, 150, 170, elastic);
        const float wrapped = rate(-179, 0, 150, 170, elastic);
        EXPECT_LT(clockwise, 0);
        EXPECT_NEAR(wrapped / clockwise, 11.0f / 9.0f, 1.0e-3f);
        EXPECT_FLOAT_EQ(rate(179, 0, 150, 180, elastic), 0);
        EXPECT_LT(rate(-179, 0, 150, 180, elastic), 0);
        EXPECT_FLOAT_EQ(rate(-179, 1, -180, 180, elastic), 1);
        const auto damper = Coefficients(0, 10);
        EXPECT_FLOAT_EQ(rate(-179, 0, 150, 170, damper), 0);
        EXPECT_LT(rate(-179, 1, 150, 170, damper), 1);
        EXPECT_FLOAT_EQ(rate(-179, -1, 150, 170, damper), -1);
    }
}

TEST(SingleTrackJointTest, HingeReadsTheZTwistOfAnArbitraryTargetAndRecoversFromTheAntipode) {
    auto joint = Joint(Kind::kHinge);
    Drive(joint.axes[5]);
    const auto expected = Turn(Vec3::sAxisZ(), 60);
    const auto tilted = Turn(Vec3::sAxisX(), 60) * expected;
    Rig rig(joint);
    rig.TargetPose(At(tilted));
    rig.Step(135);
    EXPECT_NEAR(TwistDegrees(rig.Rotation(), Vec3::sAxisZ()), 60, 0.02f);
    rig.TargetPose(At(Quat(1, 0, 0, 0)));
    EXPECT_TRUE(rig.track->IsRotationSingular());
    rig.Velocity(Vec3::sZero(), Vec3::sZero());
    rig.Step();
    ExpectVector(rig.Angular(), Vec3::sZero());
    rig.TargetPose(At(Quat::sIdentity()));
    EXPECT_FALSE(rig.track->IsRotationSingular());
    rig.Step();
    EXPECT_LT(rig.Angular().GetZ(), 0);
}

TEST(SingleTrackJointTest, ConeHasExactEndpointsAndInteriorHalfAngles) {
    auto joint = Joint(Kind::kPoint);
    joint.cone = Hard(0, 0);
    Rig zero(joint, {.b = At(Quat(0, 0, 1, 0))});
    zero.Step();
    EXPECT_LT(ConeAngle(zero.Rotation()), 179);
    zero.Step(90);
    EXPECT_LT(ConeAngle(zero.Rotation()), 0.001f);

    for (float angle : {0.25f, 30.0f, 179.75f}) {
        SCOPED_TRACE(angle);
        joint.cone = Hard(0, Rad(angle));
        Rig rig(joint, {.b = At(Turn(Vec3::sAxisY(), angle + std::min(5.0f, (180 - angle) * 0.5f)))});
        rig.Step(100);
        EXPECT_NEAR(ConeAngle(rig.Rotation()), angle, 0.001f);
    }
    joint.cone = Hard(0, Rad(0.00005f));
    Rig tiny(joint, {.b = At(Turn(Vec3::sAxisY(), 0.00002f))});
    tiny.Velocity(Vec3::sZero(), 1.0e-4f * Vec3::sAxisY());
    tiny.Step();
    EXPECT_NEAR(tiny.Angular().GetY(), Rad(0.00003f) / kDt, 2.0e-7f);
    for (auto fully_open : {Hard(0, kPi), Soft(0, kPi, Frequency(3))}) {
        joint.cone = fully_open;
        Rig rig(joint, {.b = At(Quat(0, 0, 1, 0))});
        rig.Step(4);
        EXPECT_NEAR(ConeAngle(rig.Rotation()), 180, 1.0e-4f);
        ExpectVector(rig.Angular(), Vec3::sZero());
    }
}

TEST(SingleTrackJointTest, AntipodalConeNormalFollowsTheRigAndOnlyStiffnessCanPushFromRest) {
    const auto turned = Turn(Vec3::sAxisX(), 67) * Turn(Vec3::sAxisZ(), -24);
    const Quat reverse(0, 0, 1, 0);
    for (const auto& frame : {Quat::sIdentity(), turned}) {
        for (const Axis cone : {Hard(0, Rad(30)), Soft(0, Rad(30), Frequency(2)),
                                Soft(0, Rad(30), Coefficients(0, 10)), Soft(0, Rad(30), Coefficients(0, 0))}) {
            auto joint = Joint(Kind::kPoint);
            joint.cone = cone;
            Rig rig(joint, {.b = At(frame * reverse), .a = At(frame)});
            rig.Step();
            const Vec3 direction = (frame.Conjugated() * rig.Rotation()).RotateAxisX();
            if (cone.limit == Limit::kHard || cone.soft_spring.HasStiffness()) {
                EXPECT_LT(direction.GetZ(), -0.01f);
                EXPECT_NEAR(direction.GetY(), 0, 1.0e-4f);
            } else {
                ExpectVector(direction, -Vec3::sAxisX());
                ExpectVector(rig.Angular(), Vec3::sZero());
            }
        }
    }
}

TEST(SingleTrackJointTest, DiagonalSwingDistinguishesAConeFromIndependentSwingCoordinates) {
    const Vec3 diagonal = Vec3(0, 1, 1).Normalized();
    auto circle = Joint(Kind::kPoint), rectangle = Joint(Kind::kPoint);
    circle.cone = Hard(0, Rad(30));
    rectangle.axes[4] = rectangle.axes[5] = Hard(Rad(-30), Rad(30));
    for (float initial : {40.0f, 42.2f}) {
        const auto rotation = At(Turn(diagonal, initial));
        Rig cone(circle, {.b = rotation}), box(rectangle, {.b = rotation});
        cone.Step(90);
        box.Step(90);
        EXPECT_NEAR(ConeAngle(cone.Rotation()), 30, 0.01f);
        EXPECT_NEAR(ConeAngle(box.Rotation()), initial, 0.001f);
    }
    rectangle.axes[4] = rectangle.axes[5] = Hard(Rad(41), Rad(150));
    Rig partial(rectangle, {.b = At(Turn(diagonal, 60))});
    partial.Step(30);
    EXPECT_NEAR(ConeAngle(partial.Rotation()), 60, 0.001f);
}

TEST(SingleTrackJointTest, PointSpringAndConeActAtTheSameTimeAndTheSpringCanLeaveTheStop) {
    for (auto cone : {Hard(0, Rad(30)), Soft(0, Rad(30), Frequency(4, 0.5f))}) {
        auto joint = Joint(Kind::kPoint);
        joint.cone = cone;
        Drive(joint.axes[4], Target::kAnimation, Frequency(3, 1));
        Rig rig(joint);
        rig.TargetPose(At(Turn(Vec3::sAxisY(), 50)));
        rig.Step(180);
        const float equilibrium = ConeAngle(rig.Rotation());
        if (cone.limit == Limit::kHard) {
            EXPECT_NEAR(equilibrium, 30, 0.2f);
        } else {
            // Under a common effective inertia equilibrium is the k-weighted average.
            EXPECT_NEAR(equilibrium, (9 * 50 + 16 * 30) / 25.0f, 0.3f);
        }
        rig.TargetPose(At(Quat::sIdentity()));
        rig.Step(120);
        EXPECT_LT(ConeAngle(rig.Rotation()), 0.05f);
    }
}

TEST(SingleTrackJointTest, SingularRotationLeavesLinearRowsActiveAndAutomaticallyResumes) {
    auto joint = Joint();
    Drive(joint.axes[0]);
    Drive(joint.axes[3]);
    Rig rig(joint, {.b = At(Quat(0, 0, 1, 0))});
    const RigidPose goal{Vec3::sAxisX(), Turn(Vec3::sAxisX(), 30)};
    rig.TargetPose(goal);
    EXPECT_TRUE(rig.track->IsRotationSingular());
    rig.Step();
    EXPECT_GT(rig.Linear().GetX(), 0);
    ExpectVector(rig.Angular(), Vec3::sZero());
    rig.Bodies().SetRotation(rig.id_b, Quat::sIdentity(), JPH::EActivation::Activate);
    EXPECT_FALSE(rig.track->IsRotationSingular());
    rig.Step();
    EXPECT_GT(rig.Angular().GetX(), 0);
    rig.TargetPose(At(Quat(0, 1, 0, 0)));
    EXPECT_TRUE(rig.track->IsRotationSingular());
    rig.Velocity(Vec3::sZero(), Vec3::sZero());
    rig.Step();
    ExpectVector(rig.Angular(), Vec3::sZero());
    rig.TargetPose(goal);
    rig.Step();
    EXPECT_GT(rig.Angular().GetX(), 0);
}

TEST(SingleTrackJointTest, SwappingEndsPreservesSymmetricGeometryAndMirroredHingeStops) {
    for (Kind kind : {Kind::kFixed, Kind::kHinge, Kind::kPoint}) {
        auto forward = Joint(kind), reversed = Joint(kind);
        const Vec3 axis = kind == Kind::kPoint ? Vec3(0, 1, 1).Normalized() : Vec3::sAxisZ();
        if (kind == Kind::kPoint) {
            forward.cone = reversed.cone = Hard(0, Rad(36));
        } else if (kind == Kind::kHinge) {
            forward.axes[5] = Hard(Rad(10), Rad(45));
            reversed.axes[5] = Hard(Rad(-45), Rad(-10));
            Drive(forward.axes[5]);
            Drive(reversed.axes[5]);
        }
        const auto start = At(Turn(axis, kind == Kind::kFixed ? 5 : 22));
        Rig a(forward, {.b = start}), b(reversed, {.b = start, .swap = true});
        if (kind == Kind::kHinge) {
            a.TargetPose(At(Turn(axis, 58)));
            b.TargetPose(At(Turn(axis, -58)));
        }
        a.Velocity(Vec3::sZero(), 4 * axis);
        b.Velocity(Vec3::sZero(), 4 * axis);
        for (int step = 0; step < 80; ++step) {
            a.Step();
            b.Step();
            EXPECT_NEAR(TwistDegrees(a.Rotation(), axis), TwistDegrees(b.Rotation(), axis), 0.05f);
        }
    }
}

TEST(SingleTrackJointTest, DampingAndZeroStiffnessHaveDistinctMotorAndOneSidedLimitResponses) {
    for (bool angular : {false, true}) {
        const unsigned slot = angular ? 3 : 0;
        for (bool soft : {false, true}) {
            auto first_rate = [&](float offset, float initial, JPH::SpringSettings spring) {
                auto joint = Joint();
                if (soft) {
                    joint.axes[slot] = Soft(-0.1f, 0.1f, spring);
                } else {
                    Drive(joint.axes[slot], Target::kRestPose, spring);
                }
                const auto start = angular ? At(Quat::sRotation(Vec3::sAxisX(), offset)) : At(offset * Vec3::sAxisX());
                Rig rig(joint, {.b = start});
                const Vec3 v = initial * Vec3::sAxisX();
                rig.Velocity(angular ? Vec3::sZero() : v, angular ? v : Vec3::sZero());
                rig.Step();
                return angular ? rig.Angular().GetX() : rig.Linear().GetX();
            };
            const auto damper = Coefficients(0, 20);
            for (float sign : {1.0f, -1.0f}) {
                EXPECT_FLOAT_EQ(first_rate(sign * 0.3f, 0, damper), 0);
                EXPECT_LT(sign * first_rate(sign * 0.3f, sign, damper), 0.9f);
                EXPECT_FLOAT_EQ(first_rate(sign * 0.3f, sign, Coefficients(0, 0)), sign);
                EXPECT_FLOAT_EQ(first_rate(sign * 0.3f, sign, Coefficients(1.0e-38f, 1.0e-38f)), sign);
                EXPECT_LT(sign * first_rate(sign * 0.3f, 0, Coefficients(30, 0)), -0.03f);
                if (soft) {
                    EXPECT_FLOAT_EQ(first_rate(sign * 0.3f, -sign, damper), -sign);
                    EXPECT_FLOAT_EQ(first_rate(sign * 0.05f, sign, damper), sign);
                    const float inward = first_rate(sign * 0.3f, -sign, Coefficients(30, 0));
                    EXPECT_NEAR(first_rate(sign * 0.3f, -sign, Coefficients(30, 10000)), inward, 1.0e-6f);
                } else {
                    EXPECT_LT(-sign * first_rate(sign * 0.3f, -sign, damper), 0.9f);
                }
            }
        }
    }
}

TEST(SingleTrackJointTest, MotorAndLimitApplyIndependentPairedReactionsAndBalanceAnAppliedLoad) {
    for (bool angular : {false, true}) {
        const unsigned slot = angular ? 3 : 0;
        auto displacement = [&](bool motor, bool limits) {
            auto joint = Joint();
            if (limits) {
                joint.axes[slot] = Soft(-0.1f, 0.1f, Coefficients(40, 5));
            }
            if (motor) {
                Drive(joint.axes[slot], Target::kRestPose, Coefficients(40, 5));
            }
            const auto start = angular ? At(Quat::sRotation(Vec3::sAxisX(), 0.3f)) : At(0.3f * Vec3::sAxisX());
            Rig rig(joint, {.b = start, .dynamic_a = true});
            rig.Step();
            const Vec3 vb = angular ? rig.Angular() : rig.Linear();
            const Vec3 va = angular ? rig.Bodies().GetAngularVelocity(rig.id_a) : rig.Bodies().GetLinearVelocity(rig.id_a);
            ExpectVector(va, -vb);
            return vb.GetX();
        };
        const auto off = displacement(false, false);
        const auto motor = displacement(true, false), soft = displacement(false, true);
        const auto both = displacement(true, true);
        EXPECT_FLOAT_EQ(off, 0);
        EXPECT_LT(motor, soft);
        EXPECT_LT(soft, -0.01f);
        EXPECT_LT(both, motor);
        auto joint = Joint();
        joint.axes[slot] = Soft(-0.1f, 0.1f, Coefficients(80, 15));
        Rig equilibrium(joint);
        for (int i = 0; i < 480; ++i) {
            if (angular) {
                equilibrium.Bodies().AddTorque(equilibrium.id_b, Vec3(8, 0, 0));
            } else {
                equilibrium.Bodies().AddForce(equilibrium.id_b, Vec3(8, 0, 0));
            }
            equilibrium.Step();
        }
        const float position = angular ? Rad(TwistDegrees(equilibrium.Rotation(), Vec3::sAxisX()))
                                       : equilibrium.Position().GetX();
        EXPECT_NEAR(position, 0.1f + 8.0f / 80.0f, 0.002f);
    }
}

TEST(SingleTrackJointTest, FrequencyIsMassIndependentWhileSpecifiedStiffnessSlowsTheHeavierBody) {
    for (bool angular : {false, true}) {
        const unsigned slot = angular ? 3 : 0;
        for (Target target : {Target::kRestPose, Target::kAnimation}) {
            for (bool frequency : {true, false}) {
                auto joint = Joint();
                Drive(joint.axes[slot], target, frequency ? Frequency(1, 0) : Coefficients(4 * kPi * kPi, 0));
                const auto start = angular ? At(Quat::sRotation(Vec3::sAxisX(), 0.2f)) : At(0.2f * Vec3::sAxisX());
                Rig light(joint, {.b = start}), heavy(joint, {.b = start, .mass = 4});
                light.Step();
                heavy.Step();
                const auto rate = [&](Rig& rig) { return angular ? rig.Angular().GetX() : rig.Linear().GetX(); };
                const auto distance = [&](Rig& rig) {
                    return angular ? Rad(TwistDegrees(rig.Rotation(), Vec3::sAxisX())) : rig.Position().GetX();
                };
                if (frequency) {
                    EXPECT_NEAR(rate(heavy), rate(light), 2.0e-5f);
                } else {
                    EXPECT_NEAR(rate(heavy) / rate(light), 0.25f, 0.01f);
                }
                heavy.Step(14);
                light.Step(14);
                if (frequency) {
                    EXPECT_NEAR(distance(heavy), distance(light), 5.0e-5f);
                } else {
                    EXPECT_GT(distance(heavy) - distance(light), 0.1f);
                }
            }
        }
    }
}

TEST(SingleTrackJointTest, DampingRatiosShowUnderdampedCriticalAndOverdampedRelease) {
    for (float dt : {kDt, 0.1f}) {
        std::array<std::vector<float>, 4> traces;
        const int steps_per_second = static_cast<int>(std::lround(1 / dt));
        unsigned index = 0;
        for (float ratio : {0.0f, 0.5f, 1.0f, 2.0f}) {
            auto joint = Joint();
            Drive(joint.axes[0], Target::kRestPose, Frequency(1, ratio));
            const auto start = At(Vec3(1, 0, 0));
            Rig rig(joint, {.b = start});
            int crossings = 0;
            float previous = 1;
            traces[index].push_back(previous);
            for (int i = 1; i <= 3 * steps_per_second; ++i) {
                rig.Step(1, dt, dt > 0.05f ? 2 : 1);
                const float pos = rig.Position().GetX();
                if (pos * previous < 0) {
                    ++crossings;
                }
                traces[index].push_back(pos);
                if (ratio >= 1) {
                    EXPECT_GE(pos, -0.0001f);
                    EXPECT_LE(pos, previous + 1.0e-6f);
                }
                previous = pos;
            }
            if (ratio == 0 && dt == kDt) {
                EXPECT_GE(crossings, 5);
            }
            if (ratio >= 1) {
                Rig fast(joint, {.b = start});
                fast.Velocity(Vec3(-25, 0, 0), Vec3::sZero());
                fast.Step(4 * steps_per_second, dt, dt > 0.05f ? 2 : 1);
                EXPECT_NEAR(fast.Position().GetX(), 0, 0.02f);
                EXPECT_NEAR(fast.Linear().GetX(), 0, 0.03f);
            }
            ++index;
        }
        // For zeta = 1/2 the successive turning points alternate sign on a decaying exponential.
        // Compare the critical tail at those same instants; an arbitrary window can start at a zero crossing.
        const auto& underdamped = traces[1];
        std::vector<size_t> peaks;
        for (size_t i = 1; i + 1 < underdamped.size(); ++i) {
            const float amplitude = std::abs(underdamped[i]);
            if (amplitude > std::abs(underdamped[i - 1]) && amplitude >= std::abs(underdamped[i + 1])) {
                peaks.push_back(i);
            }
        }
        ASSERT_GE(peaks.size(), 3u);
        EXPECT_LT(underdamped[peaks[0]], 0);
        EXPECT_GT(underdamped[peaks[1]], 0);
        EXPECT_LT(underdamped[peaks[2]], 0);
        for (size_t j : {1u, 2u}) {
            const auto i = peaks[j];
            const auto span = static_cast<size_t>(std::ceil(0.25f / dt));
            float low_damping_peak = 0;
            for (size_t n = i - span; n <= i + span; ++n) {
                low_damping_peak = std::max(low_damping_peak, std::abs(traces[0][n]));
            }
            const float amplitude = std::abs(underdamped[i]);
            EXPECT_LT(amplitude, std::abs(underdamped[peaks[j - 1]]));
            EXPECT_LT(amplitude, low_damping_peak);
            EXPECT_GT(amplitude, traces[2][i]);
        }
        EXPECT_GT(traces[3][steps_per_second / 2], traces[2][steps_per_second / 2] + 0.1f);
    }
}

TEST(SingleTrackJointTest, AnimationMovesOnlyEquilibriumAndRestTranslationFollowsTheAScale) {
    auto joint = Joint();
    Drive(joint.axes[0], Target::kAnimation, Coefficients(10, 100));
    Rig moving(joint);
    moving.Velocity(Vec3::sAxisX(), Vec3::sZero());
    moving.TargetPose(At(kDt * Vec3::sAxisX()));
    moving.Step();
    EXPECT_LT(moving.Linear().GetX(), 0.5f);
    EXPECT_GT(moving.Linear().GetX(), 0);

    joint.rest_target.position = 0.25f * Vec3::sAxisY();
    joint.axes[0] = {};
    Drive(joint.axes[1], Target::kRestPose);
    Rig scaled(joint, {.scale = 2});
    scaled.Step(180);
    EXPECT_NEAR(scaled.Position().GetY(), 0.5f, 0.001f);
    scaled.track->SetFrames(joint.frames, 1);
    scaled.Step(180);
    EXPECT_NEAR(scaled.Position().GetY(), 0.25f, 0.001f);

    joint.axes[1] = Soft(-0.1f, 0.1f, Coefficients(30, 0));
    Rig wide(joint, {.b = At(0.15f * Vec3::sAxisY()), .scale = 2});
    wide.Step();
    EXPECT_FLOAT_EQ(wide.Linear().GetY(), 0);
    wide.track->SetFrames(joint.frames, 1);
    wide.Step();
    EXPECT_LT(wide.Linear().GetY(), 0);
    joint.axes[1] = {};
    Drive(joint.axes[1]);
    Rig animation(joint, {.scale = 2});
    animation.TargetPose(At(0.25f * Vec3::sAxisY()));
    animation.Step(180);
    EXPECT_NEAR(animation.Position().GetY(), 0.25f, 0.001f);
}

TEST(SingleTrackJointTest, LinearDampingIncludesTheRotationOfTheMeasuringFrame) {
    auto joint = Joint();
    Drive(joint.axes[1], Target::kRestPose, Coefficients(0, 30));
    for (float speed : {0.0f, 1.0f}) {
        Rig rig(joint, {.b = At(Vec3::sAxisX()), .dynamic_a = true});
        rig.Bodies().SetAngularVelocity(rig.id_a, Vec3::sAxisZ());
        rig.Velocity(speed * Vec3::sAxisY(), Vec3::sZero());
        rig.Step();
        if (speed > 0) {
            ExpectVector(rig.Linear(), Vec3::sAxisY());
            ExpectVector(rig.Bodies().GetAngularVelocity(rig.id_a), Vec3::sAxisZ());
        } else {
            EXPECT_GT(rig.Linear().GetY(), 0.1f);
            EXPECT_LT(rig.Bodies().GetAngularVelocity(rig.id_a).GetZ(), 0.9f);
        }
    }
}

TEST(SingleTrackJointTest, SixHertzSpringUnderLoadAndContactRecoversAcrossTheCoarseStepWindow) {
    for (Kind kind : {Kind::kHinge, Kind::kPoint}) {
        for (bool soft : {false, true}) {
            auto joint = Joint(kind);
            joint.frames[1].position = Vec3::sAxisY();
            if (soft) {
                if (kind == Kind::kHinge) {
                    joint.axes[5] = Soft(Rad(-4), Rad(4), Frequency(3, 0.5f));
                } else {
                    joint.cone = Soft(0, Rad(4), Frequency(3, 0.5f));
                }
            }
            Drive(joint.axes[5], Target::kAnimation, Frequency(6, 0.7f));
            constexpr float wall_gap = 0.14f;
            Rig rig(joint, {.b = At(-Vec3::sAxisY())}), free(joint, {.b = At(-Vec3::sAxisY())});
            rig.AddWall(std::sqrt(2.5f) + wall_gap);
            for (auto* loaded : {&rig, &free}) {
                loaded->SetGravity(Vec3(0, -9.81f, 0));
                loaded->TargetPose(At(Turn(Vec3::sAxisZ(), 30)));
            }
            bool hit = false;
            for (int i = 0; i < 16; ++i) {
                rig.Step(1, 0.1f, 2);
                free.Step(1, 0.1f, 2);
                hit |= rig.ContactWithWall();
                EXPECT_LT(rig.Linear().Length(), 20);
                EXPECT_LT(rig.Angular().Length(), 20);
            }
            EXPECT_TRUE(hit);
            // A unit lever and unit rotor inertia give k_m (goal-theta) - k_s (theta-limit) = mg sin(theta).
            const float motor_k = std::pow(2 * kPi * 6, 2);
            const float limit_k = soft ? std::pow(2 * kPi * 3, 2) : 0;
            float equilibrium = Rad(30);
            for (unsigned i = 0; i < 4; ++i) {
                equilibrium = (motor_k * Rad(30) + limit_k * Rad(4) - 9.81f * std::sin(equilibrium)) / (motor_k + limit_k);
            }
            EXPECT_NEAR(TwistDegrees(free.Rotation(), Vec3::sAxisZ()), Deg(equilibrium), 0.15f);
            // Discrete contact can first be found after crossing the face at 50 ms. Test the loaded
            // resting center against the sphere/plane tangent and the production penetration slop.
            EXPECT_NEAR(rig.Position().GetX(), wall_gap, 2 * kContactSlop);
            EXPECT_TRUE(rig.ContactWithWall());
            rig.TargetPose(At(Quat::sIdentity()));
            rig.Step(180);
            EXPECT_NEAR(TwistDegrees(rig.Rotation(), Vec3::sAxisZ()), 0, 0.1f);
            EXPECT_LT(rig.Linear().Length(), 0.02f);
            EXPECT_FALSE(rig.ContactWithWall());
        }
    }
}

}  // namespace
}  // namespace ysm::physics
