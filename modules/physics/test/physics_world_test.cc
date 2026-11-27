#include "physics_test_rig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <thread>

#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <gtest/gtest.h>
#include <mimalloc.h>

#include "physics/runtime.h"
#include "proto/asset/model/data/geo_model.proto.h"

namespace ysm::physics {

namespace {
using test::Ball;
using test::DebugSnapshot;
using test::Link;
using Kind = schema::PhysicsConstraintKind;
using NodeKind = schema::PhysicsNodeKind;
using Limit = schema::LimitKind;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kStep = 1.0f / 60;
static_assert(sizeof(JPH::PhysicsUpdateContext::Step) <= 16 * 1024);

std::shared_ptr<const PhysicsAsset> Asset(std::vector<schema::PhysicsNode> nodes,
                                          std::vector<schema::PhysicsConstraint> joints,
                                          float gravity_y = 0) {
    InitializeRuntime();
    schema::GeoModel geo;
    for (const auto& n : nodes) {
        auto& bone = geo.bones.emplace_back();
        bone.name = *n.bone;
        bone.pivot.resize(3);
        bone.rotate.resize(3);
    }
    schema::PhysicsDefinition def{.nodes = std::move(nodes), .constraints = std::move(joints),
                                  .gravity = {0, gravity_y, 0}};
    std::string geo_bytes, def_bytes;
    struct_pb::to_pb(geo, geo_bytes);
    struct_pb::to_pb(def, def_bytes);
    return CompilePhysicsAsset(StrBuf(geo_bytes), StrBuf(def_bytes)).value();
}

struct Rig {
    explicit Rig(const std::shared_ptr<const PhysicsAsset>& asset)
        : world(PhysicsWorld::Create(asset).value()), engine(PhysicsWorldTestAccess::Get(world)),
          targets(engine.bodies.size()), frame{.nodes = targets} {}
    void Reset() { ASSERT_EQ(world.Reset(frame).outcome, StepOutcome::kSuccess); }
    void Step(float dt = kStep) { ASSERT_EQ(world.Step(frame, dt).outcome, StepOutcome::kSuccess); }
    JPH::Body& Body(size_t i) { return *engine.bodies[i].body; }
    PhysicsWorld world;
    decltype(PhysicsWorldTestAccess::Get(world)) engine;
    std::vector<PhysicsNodeTarget> targets;
    PhysicsFrame frame;
};

std::array<float, 4> ZRotation(float angle) {
    return {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
}

float ZAngle(const PhysicsPose& pose) { return 2 * std::atan2(pose.rotation[2], pose.rotation[3]); }

TEST(PhysicsWorldTest, SinglePointHingeFixesDeclaredAngle) {
    for (float angle : {0.0f, kPi / 6}) {
        auto link = Link(Kind::kHinge, "anchor", "body");
        link.angular_z = schema::AxisConfig{.limit = Limit::kHard, .min = angle, .max = angle};
        Rig rig(Asset({Ball("body"), Ball("anchor", NodeKind::kAnchor)}, {link}));
        rig.Reset();
        ASSERT_EQ(rig.engine.joints[0].native->GetSubType(), JPH::EConstraintSubType::Fixed);
        rig.Body(0).SetAngularVelocity(JPH::Vec3(0.5f, -0.25f, 3));
        for (int i = 0; i < 120; ++i) rig.Step();
        EXPECT_NEAR(ZAngle(rig.world.ReadNodePivot(0)), angle, 0.001f);
        EXPECT_LT(rig.Body(0).GetAngularVelocity().Length(), 0.001f);
    }
}

TEST(PhysicsWorldTest, ThreeAxisEqualityFollowsRotatingScaledReferenceAttachment) {
    auto generic = Link(Kind::kGeneric, "anchor", "body");
    generic.linear_x = schema::AxisConfig{.limit = Limit::kHard, .min = 16, .max = 16};
    generic.linear_y = generic.linear_z = schema::AxisConfig{.limit = Limit::kLocked};
    auto point = Link(Kind::kPoint, "anchor", "body");
    point.frame_a.pivot_offset[0] = 16;
    Rig equality(Asset({Ball("body"), Ball("anchor", NodeKind::kAnchor)}, {generic}));
    Rig attachment(Asset({Ball("body"), Ball("anchor", NodeKind::kAnchor)}, {point}));
    for (auto* rig : {&equality, &attachment}) {
        rig->targets[0].pivot.position[0] = 1;
        rig->Reset();
    }
    for (int i = 1; i <= 120; ++i) {
        const float angle = i * kPi / 240;
        for (auto* rig : {&equality, &attachment}) {
            rig->targets[1].pivot.rotation = ZRotation(angle);
            rig->targets[1].scale = i <= 55 ? 1 : 1.5f;
            rig->Step();
        }
        const auto v = equality.Body(0).GetLinearVelocity();
        EXPECT_LT((v - attachment.Body(0).GetLinearVelocity()).Length(), 2e-4f);
    }
    const auto p = equality.world.ReadNodePivot(0).position;
    EXPECT_NEAR(p[0], 0, 0.005f);
    EXPECT_NEAR(p[1], 1.5f, 0.005f);
    EXPECT_NEAR(equality.Body(0).GetLinearVelocity().GetX(), -1.5f * kPi / 4, 0.02f);
}

TEST(PhysicsWorldTest, RescalePreservesPivotAndVelocityAndMovesEachNativeAttachment) {
    auto first = Link(Kind::kPoint, "body", "a");
    auto second = Link(Kind::kHinge, "body", "b");
    first.frame_a.pivot_offset = {-8, 8, 0};
    second.frame_a.pivot_offset = {0, 4, 0};
    auto ball = Ball("body", NodeKind::kDynamic, 0.25f, {2, 0, 0});
    ball.mass = 0.005f;
    Rig rig(Asset({ball, Ball("a", NodeKind::kAnchor), Ball("b", NodeKind::kAnchor)}, {first, second}));
    rig.targets[0].pivot.rotation = ZRotation(kPi / 4);
    rig.Reset();
    auto& b = rig.Body(0);
    const auto pos = rig.engine.Pivot(0).position;
    const JPH::Vec3 v(1, -0.5f, 0), w(-1, 0.3f, 0.1f);
    b.SetLinearVelocity(v);
    b.SetAngularVelocity(w);
    const auto inv = 1.0f / (0.4f * 0.005f * std::pow(0.25f / 16, 2.0f));
    const auto* first_ptr = rig.engine.joints[0].native.GetPtr();
    for (float s : {2.0f, 1.0f}) {
        rig.engine.RescaleBody(0, s);
        EXPECT_LT((rig.engine.Pivot(0).position - pos).Length(), 2e-6f);
        EXPECT_EQ(b.GetLinearVelocity(), v);
        EXPECT_EQ(b.GetAngularVelocity(), w);
        EXPECT_NEAR(b.GetMotionProperties()->GetInverseInertiaDiagonal().GetX(), inv / (s * s), 1.0f);
        EXPECT_FLOAT_EQ(b.GetMotionProperties()->GetInverseMass(), 200);
        EXPECT_EQ(rig.engine.joints[0].native.GetPtr(), first_ptr);
        const auto p1 = rig.engine.joints[0].native->GetConstraintToBody1Matrix().GetTranslation();
        const auto p2 = rig.engine.joints[1].native->GetConstraintToBody1Matrix().GetTranslation();
        EXPECT_LT((p1 - s * JPH::Vec3(-10.0f/16, 8.0f/16, 0)).Length(), 2e-6f);
        EXPECT_LT((p2 - s * JPH::Vec3(-2.0f/16, 4.0f/16, 0)).Length(), 2e-6f);
    }
}

TEST(PhysicsWorldTest, PendulumPeriodMatchesSmallAngleRigidBodyPeriod) {
    auto link = Link(Kind::kPoint, "pin", "bob");
    Rig rig(Asset({Ball("bob", NodeKind::kDynamic, 1, {0, -16, 0}),
                   Ball("pin", NodeKind::kAnchor)}, {link}, -9.81f));
    rig.targets[0].pivot.rotation = ZRotation(0.1f);
    rig.Reset();
    float previous = rig.Body(0).GetCenterOfMassPosition().GetX();
    std::vector<float> crossings;
    for (int i = 1; i <= 850; ++i) {
        rig.Step(1.0f/240);
        const auto x = rig.Body(0).GetCenterOfMassPosition().GetX();
        if (previous >= 0 && x < 0) {
            crossings.push_back((i - 1 + previous / (previous - x)) / 240);
        }
        previous = x;
    }
    ASSERT_GE(crossings.size(), 2u);
    const float period = 2 * kPi * std::sqrt((1 + 0.4f / 256) / 9.81f);
    EXPECT_NEAR(crossings[1] - crossings[0], period, period * 0.02f);
}

TEST(PhysicsWorldTest, ExponentialDragCombinesMaterialsAndRetainsImmediateAnimationResponse) {
    auto ball = Ball("bob");
    ball.linear_damping = ball.angular_damping = 10;
    ball.friction = 1e30f;
    ball.bounciness = 0.25f;
    auto proxy = Ball("proxy", NodeKind::kProxy);
    proxy.friction = 1e20f;
    proxy.bounciness = 0.8f;
    Rig rig(Asset({ball, proxy}, {}));
    rig.targets[1].pivot.position[0] = 5;
    for (int n : {6, 1}) {
        rig.Reset();
        rig.Body(0).SetLinearVelocity(JPH::Vec3::sAxisX());
        rig.Body(0).SetAngularVelocity(JPH::Vec3::sAxisY());
        for (int i = 0; i < n; ++i) rig.Step(0.1f / n);
        EXPECT_NEAR(rig.Body(0).GetLinearVelocity().GetX(), std::exp(-1.0f), 0.0036f);
        EXPECT_NEAR(rig.Body(0).GetAngularVelocity().GetY(), std::exp(-1.0f), 0.0036f);
    }
    EXPECT_NEAR(rig.engine.system.GetCombineFriction()(rig.Body(0), {}, rig.Body(1), {}), 1e25f, 1e19f);
    EXPECT_FLOAT_EQ(rig.engine.system.GetCombineRestitution()(rig.Body(0), {}, rig.Body(1), {}), 0.8f);

    auto motor = Link(Kind::kHinge, "pin", "bob");
    motor.angular_z = schema::AxisConfig{.spring = schema::SpringConfig{
        schema::SpringTarget::kAnimation, schema::SpringParameters{schema::FrequencyDamping{1, 0.5f}}}};
    Rig follower(Asset({Ball("bob"), Ball("pin", NodeKind::kAnchor)}, {motor}));
    follower.Reset();
    for (int i = 0; i < 245; ++i) follower.Step();
    EXPECT_TRUE(follower.Body(0).IsActive());
    follower.targets[0].pivot.rotation = ZRotation(0.25f);
    follower.Step();
    EXPECT_GT(follower.Body(0).GetAngularVelocity().GetZ(), 0.01f);
}

TEST(PhysicsWorldTest, DebugCaptureReportsWholeStepAndSamplingHistory) {
    Rig rig(Asset({Ball("pin", NodeKind::kAnchor), Ball("proxy", NodeKind::kProxy), Ball("ball")}, {}));
    const auto expect = [&](int32_t flags, ContactState state) {
        const DebugSnapshot snapshot(rig.world);
        ASSERT_EQ(snapshot.flags, flags);
        if (flags != 0) {
            ASSERT_EQ(snapshot.ints.size(), 12u);
            EXPECT_EQ(snapshot.ints[9], static_cast<int32_t>(ContactState::kIneligible));
            EXPECT_EQ(snapshot.ints[10], static_cast<int32_t>(ContactState::kIneligible));
            EXPECT_EQ(snapshot.ints[11], static_cast<int32_t>(state));
        }
    };
    constexpr auto sample = DebugSnapshotOut::kSample;
    constexpr auto stepped = sample | DebugSnapshotOut::kStepped;
    constexpr auto sampled = stepped | DebugSnapshotOut::kContactSampled;
    expect(0, ContactState::kNoSample);
    EXPECT_EQ(rig.engine.system.GetContactListener(), nullptr);
    EXPECT_FALSE(rig.engine.capture);
    rig.targets[0].pivot.position[0] = 2;
    rig.Reset();
    rig.Step();
    rig.world.SetDebugContactCapture(true);
    expect(stepped, ContactState::kNotSampled);
    rig.Step();
    expect(sampled, ContactState::kNoManifold);
    rig.targets[0].pivot.position[0] = 1.015f;
    rig.Reset();
    expect(sample, ContactState::kNoStep);
    rig.Step();
    expect(sampled, ContactState::kSpeculative);
    rig.Body(0).SetLinearVelocity(-10 * JPH::Vec3::sAxisX());
    rig.Step();
    expect(sampled, ContactState::kSpeculative);
    EXPECT_NEAR(rig.Body(0).GetPosition().GetX(), 1, 0.002f);
    rig.targets[0].pivot.position[0] = 0.99f;
    rig.Reset();
    rig.Body(0).SetLinearVelocity(10 * JPH::Vec3::sAxisX());
    rig.Step(0.1f);
    EXPECT_GT(rig.Body(0).GetPosition().GetX(), 1.5f);
    expect(sampled, ContactState::kTouching);
    rig.world.SetDebugContactCapture(false);
    EXPECT_EQ(rig.engine.system.GetContactListener(), nullptr);
    EXPECT_FALSE(rig.engine.capture);
    rig.Step();
    rig.world.SetDebugContactCapture(true);
    expect(stepped, ContactState::kNotSampled);
}

TEST(PhysicsWorldTest, DebugSnapshotKeepsAuthoredAttachmentsAndDoesNotChangeDeltas) {
    auto equality = Link(Kind::kGeneric, "pin", "slide");
    equality.frame_a.pivot_offset = {16, 0, 0};
    equality.linear_x = schema::AxisConfig{.limit = Limit::kHard, .min = 16, .max = 16};
    equality.linear_y = equality.linear_z = schema::AxisConfig{.limit = Limit::kLocked};
    auto hinge = Link(Kind::kHinge, "pin", "twist");
    hinge.angular_z = schema::AxisConfig{.limit = Limit::kHard, .min = kPi / 6, .max = kPi / 6};
    Rig rig(Asset({Ball("pin", NodeKind::kAnchor), Ball("slide"), Ball("twist")}, {equality, hinge}));
    rig.targets[0] = {PhysicsPose{{1, 6, 0}, ZRotation(kPi / 2)}, 1};
    rig.targets[1] = {PhysicsPose{{1, 2, 0}, ZRotation(kPi / 2 + kPi / 6)}, 1};
    rig.targets[2] = {PhysicsPose{{1, 2, 0}, ZRotation(kPi / 2)}, 2};
    rig.frame.root_rotation = ZRotation(-kPi / 2);
    rig.frame.root_position = {3e7, 7, -3e7};
    rig.Reset();
    const auto deltas = rig.world.ActiveDeltas();
    ASSERT_EQ(deltas.size(), 2u);
    std::vector<std::byte> before(deltas.size_bytes());
    std::ranges::copy(std::as_bytes(deltas), before.begin());
    const DebugSnapshot snapshot(rig.world);
    ASSERT_EQ(snapshot.flags, DebugSnapshotOut::kSample);
    EXPECT_EQ(std::memcmp(before.data(), rig.world.ActiveDeltas().data(), before.size()), 0);

    const auto wire = std::span(snapshot.floats);
    EXPECT_NEAR(wire[0], 1, 1e-5f);
    EXPECT_NEAR(wire[1], 2, 1e-5f);
    EXPECT_NEAR(wire[9], 6, 1e-5f);
    EXPECT_EQ(wire[7], 2);
    const auto equality_reading = wire.subspan(24, 33);
    const auto hinge_reading = wire.subspan(57, 33);
    EXPECT_EQ(snapshot.ints[9], 15);
    EXPECT_EQ(snapshot.ints[10], 15);
    EXPECT_NEAR(equality_reading[0], 1, 1e-5f);
    EXPECT_NEAR(equality_reading[1], 4, 1e-5f);
    EXPECT_NEAR(equality_reading[5], std::sin(kPi / 4), 1e-5f);
    EXPECT_NEAR(equality_reading[8], 6, 1e-5f);
    EXPECT_EQ(equality_reading[14], 2);
    EXPECT_NEAR(equality_reading[15], 2, 1e-5f);
    EXPECT_NEAR(equality_reading[21], -1, 1e-5f);
    EXPECT_NEAR(equality_reading[27], 2, 1e-5f);
    ASSERT_EQ(rig.engine.joints[1].native->GetSubType(), JPH::EConstraintSubType::Fixed);
    EXPECT_NEAR(hinge_reading[20], kPi / 6, 1e-5f);
    EXPECT_NEAR(hinge_reading[32], kPi / 6, 1e-5f);
}

TEST(PhysicsWorldTest, DebugSnapshotMarksCurrentAndTargetRotationSingularIndependently) {
    const auto joint = Link(Kind::kGeneric, "pin", "body");
    Rig rig(Asset({Ball("body"), Ball("pin", NodeKind::kAnchor)}, {joint}));
    rig.Reset();
    rig.targets[0].pivot.rotation = {0, 1, 0, 0};
    rig.Step();
    const DebugSnapshot target_only(rig.world);
    EXPECT_EQ(target_only.ints[6], 15 | 64);
    rig.Reset();
    const DebugSnapshot both(rig.world);
    EXPECT_EQ(both.ints[6], 15 | 16 | 64);
    EXPECT_TRUE(std::ranges::all_of(both.floats, [](float v) { return std::isfinite(v); }));
}

TEST(PhysicsWorldTest, RebaseUsesWorldDoublePrecisionAndProductionStepBands) {
    Rig rig(Asset({Ball("drift")}, {}));
    rig.frame.root_position = {3e7, 0, -3e7};
    rig.Reset();
    for (double dx : {0.125, 8.0, 8.125}) {
        rig.frame.root_position[0] = 3e7 + dx;
        rig.Step();
        EXPECT_NEAR(rig.world.ReadNodePivot(0).position[0], -dx, 2e-6);
        EXPECT_NEAR(DebugSnapshot(rig.world).floats[0], -dx, 2e-6);
    }
    EXPECT_EQ(rig.engine.origin, rig.frame.root_position);
    for (const auto [dt, n] : {std::pair{1.0f/30,1}, {0.049f,1}, {0.05f,1}, {0.051f,2},
                                {0.1f,2}, {0.15f,3}, {0.249f,4}, {0.25f,4}}) {
        EXPECT_EQ(rig.engine.CollisionSteps(dt), n);
    }
}

TEST(PhysicsWorldTest, ThreeBodyLoopTransfersMotionAndRecoversAfterLowFrequencyUpdates) {
    auto spring = [](const char* a, const char* b, float hz) {
        auto link = Link(Kind::kGeneric, a, b);
        link.linear_x = schema::AxisConfig{.spring = schema::SpringConfig{
            schema::SpringTarget::kRestPose, schema::SpringParameters{schema::FrequencyDamping{hz, 0.5f}}}};
        link.linear_y = link.linear_z = schema::AxisConfig{.limit = Limit::kLocked};
        return link;
    };
    auto nodes = std::vector{Ball("left"), Ball("right"), Ball("pin", NodeKind::kAnchor)};
    nodes[0].collection_mask = 0;
    auto legs = std::vector{spring("pin", "left", 1), spring("pin", "right", 1)};
    Rig open(Asset(nodes, legs));
    legs.push_back(spring("left", "right", 6));
    Rig closed(Asset(nodes, legs));
    for (auto* rig : {&open, &closed}) {
        rig->targets[0].pivot.position[0] = 0.5f;
        rig->Reset();
    }
    for (int i = 0; i < 12; ++i) {
        open.Step(0.1f);
        closed.Step(0.1f);
        EXPECT_FLOAT_EQ(open.Body(1).GetPosition().GetX(), 0);
        if (i == 0) {
            EXPECT_GT(closed.Body(1).GetPosition().GetX(), 0.02f);
        }
        EXPECT_LT(closed.Body(0).GetPosition().Length(), 1);
        EXPECT_LT(closed.Body(1).GetPosition().Length(), 1);
    }
    for (int i = 0; i < 120; ++i) closed.Step();
    EXPECT_LT(closed.Body(0).GetPosition().Length(), 0.002f);
    EXPECT_LT(closed.Body(1).GetPosition().Length(), 0.002f);
}

TEST(PhysicsWorldTest, CollisionPermissionUsesTwentyCollectionsAndEveryDisableEdge) {
    auto a = Ball("a");
    auto b = Ball("b");
    const auto epoch = [&](std::vector<schema::PhysicsConstraint> edges, ContactState expected) {
        Rig rig(Asset({a, b}, std::move(edges)));
        rig.targets[1].pivot.position[0] = 0.99f;
        rig.Reset();
        for (const auto& joint : rig.engine.joints) {
            EXPECT_FALSE(joint.installed);
            EXPECT_FALSE(joint.native);
            EXPECT_FALSE(joint.custom);
        }
        rig.world.SetDebugContactCapture(true);
        rig.Step();
        EXPECT_EQ(rig.world.ReadContact(0, 1), expected);
    };
    b.collection_mask = 0;
    epoch({}, ContactState::kIneligible);
    b.collection_mask = 1u << 19;
    epoch({}, ContactState::kIneligible);
    a.collection_mask = (1u << 19) | 1;
    epoch({}, ContactState::kTouching);
    auto empty = Link(Kind::kGeneric, "a", "b");
    for (int mode = 0; mode < 4; ++mode) {
        if (mode == 1) {
            empty.angular_x = schema::AxisConfig{.limit = Limit::kHard, .min = -kPi, .max = kPi};
        } else if (mode == 2) {
            empty.angular_x = schema::AxisConfig{.limit = Limit::kSoft, .min = -0.1f, .max = 0.1f,
                .soft_limit_spring = schema::SpringParameters{schema::StiffnessDamping{0, 0}}};
        } else if (mode == 3) {
            empty.angular_x.reset();
            empty.angular_z = schema::AxisConfig{.limit = Limit::kSoft, .min = -kPi, .max = kPi,
                .soft_limit_spring = schema::SpringParameters{schema::FrequencyDamping{2, 1}}};
        }
        empty.disable_collisions = false;
        epoch({empty}, ContactState::kTouching);
        auto veto = empty;
        veto.id += "-veto";
        veto.disable_collisions = true;
        epoch({empty, veto}, ContactState::kIneligible);
    }
}

TEST(PhysicsWorldTest, FailureWithdrawsOnlyTheAffectedWorldAndScratchCanBeReused) {
    const auto asset = Asset({Ball("dynamic")}, {});
    Rig failed(asset), other(asset);
    failed.Reset();
    other.Reset();
    const auto failed_prepare = failed.engine.Advance(failed.frame, kStep, []() -> WorkerScratch& {
        throw std::bad_alloc();
    });
    EXPECT_EQ(failed_prepare.outcome, StepOutcome::kScratchExhausted);
    EXPECT_FALSE(failed.world.HasSample());
    const auto required = failed.engine.scratch_bytes;
    failed.engine.scratch_bytes = std::numeric_limits<size_t>::max();
    EXPECT_EQ(failed.world.Step(failed.frame, kStep).outcome, StepOutcome::kScratchExhausted);
    EXPECT_FALSE(failed.world.HasSample());
    failed.engine.scratch_bytes = required;
    WorkerScratch tiny(32);
    const auto borrowed = other.engine.Advance(other.frame, kStep, [&]() -> WorkerScratch& { return tiny; });
    EXPECT_EQ(borrowed.outcome, StepOutcome::kSuccess);
    EXPECT_GT(borrowed.peak_bytes, 32u);
    EXPECT_TRUE(other.world.HasSample());
    for (bool angular : {false, true}) {
        failed.Reset();
        auto* motion = failed.Body(0).GetMotionProperties();
        const JPH::Vec3 nan(std::numeric_limits<float>::quiet_NaN(), 0, 0);
        if (angular) {
            motion->ApplyAngularVelocityStep(JPH::Vec3(std::numeric_limits<float>::infinity(), 0, 0));
        } else {
            motion->MoveKinematic(nan, JPH::Quat::sIdentity(), 1.0f);
        }
        EXPECT_EQ(failed.engine.Finish(JPH::EPhysicsUpdateError::None, 0).outcome,
                  StepOutcome::kNonFiniteState);
        EXPECT_FALSE(failed.world.HasSample());
        other.Step();
    }
}

TEST(PhysicsWorldTest, EngineCapacityFailureLeavesTheNextEntityUsable) {
    std::vector<schema::PhysicsNode> dense;
    for (int i = 0; i < 50; ++i) dense.push_back(Ball(std::to_string(i)));
    Rig crowded(Asset(std::move(dense), {}));
    crowded.Reset();
    const auto result = crowded.world.Step(crowded.frame, kStep);
    EXPECT_EQ(result.outcome, StepOutcome::kCapacityExceeded);
    EXPECT_NE(result.engine_error, 0u);
    EXPECT_LE(result.peak_bytes, crowded.engine.scratch_bytes);
    EXPECT_FALSE(crowded.world.HasSample());
    Rig other(Asset({Ball("loose")}, {}));
    other.Reset();
    other.Step();
    EXPECT_TRUE(other.world.HasSample());
}

TEST(PhysicsWorldTest, MaximumRigidTopologyFitsThePreparedFourSubstepWorkspace) {
    std::vector<schema::PhysicsNode> nodes;
    std::vector<schema::PhysicsConstraint> joints;
    nodes.push_back(Ball("pin", NodeKind::kAnchor));
    for (int i = 1; i < kMaxNodes; ++i) {
        auto node = Ball(std::to_string(i));
        node.collection_mask = 0;
        nodes.push_back(std::move(node));
    }
    for (int i = 0; i < kMaxConstraints; ++i) {
        auto joint = Link(Kind::kPoint, "pin", std::to_string(i % (kMaxNodes - 1) + 1));
        joint.id = std::to_string(i);
        joint.angular_x = schema::AxisConfig{.limit = Limit::kLocked};
        joints.push_back(std::move(joint));
    }
    Rig rig(Asset(std::move(nodes), std::move(joints)));
    rig.Reset();
    const auto sample = rig.world.Step(rig.frame, 0.249f);
    EXPECT_EQ(sample.outcome, StepOutcome::kSuccess);
    EXPECT_GT(sample.peak_bytes, 0u);
    EXPECT_LE(sample.peak_bytes, rig.engine.scratch_bytes);
    EXPECT_LE(rig.engine.scratch_bytes, FallbackTempAllocator::kStackBytes);
    RecordProperty("peak_bytes", std::to_string(sample.peak_bytes));
    RecordProperty("reserved_bound_bytes", std::to_string(rig.engine.scratch_bytes));
}

TEST(PhysicsWorldTest, ThreadScratchReturnsOverflowAndSupportsThreadExit) {
    bool completed = false;
    std::thread thread([&] {
        auto& scratch = WorkerScratch::Current();
        scratch.Prepare(72);
        ASSERT_TRUE(scratch.temp.BeginStep(FallbackTempAllocator::kStackBytes));
        auto* small = scratch.temp.Allocate(64);
        EXPECT_FALSE(mi_is_in_heap_region(small));
        EXPECT_FALSE(mi_is_in_heap_region(scratch.fk_bones.data()));
        scratch.temp.Free(small, 64);
        scratch.temp.EndStep();
        constexpr auto whole_step = FallbackTempAllocator::kStackBytes + 64;
        ASSERT_TRUE(scratch.temp.BeginStep(whole_step));
        auto* large = scratch.temp.Allocate(whole_step);
        EXPECT_TRUE(mi_is_in_heap_region(large));
        scratch.temp.Free(large, whole_step);
        scratch.temp.EndStep();
        bool ran = false;
        auto job = scratch.jobs.CreateJob("worker", JPH::Color::sRed, [&] { ran = true; }, 0);
        completed = ran && job.IsDone();
    });
    thread.join();
    EXPECT_TRUE(completed);
}

}  // namespace
}  // namespace ysm::physics
