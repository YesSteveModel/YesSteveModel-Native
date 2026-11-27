#include "physics_test_rig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

#include <gtest/gtest.h>

#include "gfx/renderer/model_state.h"
#include "physics/runtime.h"
#include "proto/asset/model/data/geo_model.proto.h"

namespace ysm::physics {
namespace {
using test::Ball;
using test::DebugSnapshot;
using test::Link;
using NodeKind = schema::PhysicsNodeKind;
using Kind = DeactivationKind;
constexpr float kDt = 1.0f / 60;
constexpr simd::Tag<simd::Type::kNone> kGeneric;

schema::Bone Bone(std::string name, std::string parent = "", std::vector<float> pivot = {0, 0, 0},
                  std::vector<float> rotation = {0, 0, 0}) {
    return {std::move(name), std::move(parent), std::move(pivot), std::move(rotation), {}, {}};
}

schema::PhysicsConstraint WideTranslation(std::string a, std::string b) {
    auto joint = Link(schema::PhysicsConstraintKind::kGeneric, std::move(a), std::move(b));
    joint.linear_x = schema::AxisConfig{.limit = schema::LimitKind::kHard, .min = -512, .max = 512};
    return joint;
}

std::string Encode(const auto& object) {
    std::string bytes;
    struct_pb::to_pb(object, bytes);
    return bytes;
}

std::shared_ptr<gfx::bake::BakedModel> Bake(const std::string& geometry) {
    InitCpuInfo();
    const gfx::bake::Pixel white{255, 255, 255, 255};
    return gfx::bake::BakeModel(StrBuf(geometry), gfx::bake::Texture(&white, 1, 1), {}).value();
}

PhysicsWorld MakeWorld(const std::string& geometry, std::vector<schema::PhysicsNode> nodes,
                       std::vector<schema::PhysicsConstraint> joints) {
    InitializeRuntime();
    const auto definition = Encode(schema::PhysicsDefinition{
        .nodes = std::move(nodes), .constraints = std::move(joints), .gravity = {0, 0, 0}});
    return PhysicsWorld::Create(CompilePhysicsAsset(StrBuf(geometry), StrBuf(definition)).value()).value();
}

struct SampleRig {
    SampleRig(std::vector<schema::Bone> bones, std::vector<schema::PhysicsNode> nodes,
              std::vector<schema::PhysicsConstraint> joints, schema::Cubes cubes = {})
        : geometry(std::move(bones), {1, 1}, std::move(cubes)), bytes(Encode(geometry)), baked(Bake(bytes)),
          world(MakeWorld(bytes, std::move(nodes), std::move(joints))), engine(PhysicsWorldTestAccess::Get(world)),
          channels(geometry.bones.size()), packed(channels.size() * gfx::renderer::BoneAttribute::kFloatCount) {
        for (size_t i = 0; i < channels.size(); ++i) {
            const auto& bind = geometry.bones[baked->Bones().sorted_bone_indices[i]].rotate;
            std::ranges::copy(bind, channels[i].rotation);
            std::ranges::fill(channels[i].scale, 1.0f);
        }
    }

    size_t Sorted(std::string_view name) const {
        const auto& order = baked->Bones().sorted_bone_indices;
        return std::ranges::find_if(order, [&](auto i) { return geometry.bones[i].name == name; }) - order.begin();
    }
    size_t Slot(std::string_view id) const {
        return std::ranges::find(engine.data.nodes, id, &Node::id) - engine.data.nodes.begin();
    }
    gfx::renderer::BoneAttribute& Channels(std::string_view name) { return channels[Sorted(name)]; }
    JPH::Body& Body(std::string_view id) { return *engine.bodies[Slot(id)].body; }
    void Scale(std::string_view name, float s) { std::ranges::fill(Channels(name).scale, s); }
    bool Gather() {
        if (!world.PrepareGather()) return false;
        std::memcpy(packed.data(), channels.data(), packed.size() * sizeof(float));
        world.GatherBoneAttributes(packed);
        return true;
    }
    StepResult Advance(bool reset) {
        if (!Gather()) return {StepOutcome::kScratchExhausted};
        return world.AdvanceGathered(reset, kDt, position, rotation);
    }
    void ExpectReason(std::string_view node, Kind kind, uint16_t culprit) const {
        const auto slot = Slot(node);
        const auto value = world.ReadDeactivationReason(slot);
        EXPECT_EQ(value.kind, kind) << node;
        EXPECT_EQ(value.culprit, culprit) << node;
        EXPECT_EQ(world.IsNodeActive(slot), kind == Kind::kActive) << node;
        const DebugSnapshot snapshot(world);
        ASSERT_TRUE(snapshot.flags & DebugSnapshotOut::kSample);
        const size_t idx = engine.data.nodes[slot].authored_index * DebugSnapshotOut::kNodeInts;
        EXPECT_EQ((snapshot.ints[idx] & 2) != 0, kind == Kind::kActive) << node;
        EXPECT_EQ(snapshot.ints[idx + 1], static_cast<int32_t>(kind)) << node;
        EXPECT_EQ(snapshot.ints[idx + 2], culprit) << node;
    }
    void Place(std::string_view node, JPH::Vec3Arg point, JPH::QuatArg rotation) {
        // All rig spheres used by this helper are centered on the bone pivot.
        const auto q = engine.root.rotation;
        engine.system.GetBodyInterfaceNoLock().SetPositionAndRotation(
            Body(node).GetID(), engine.root.position + q * point, q * rotation, JPH::EActivation::Activate);
    }
    schema::GeoModel geometry;
    std::string bytes;
    std::shared_ptr<gfx::bake::BakedModel> baked;
    PhysicsWorld world;
    decltype(PhysicsWorldTestAccess::Get(world)) engine;
    std::vector<gfx::renderer::BoneAttribute> channels;
    std::vector<float> packed;
    std::array<double, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
};

void ExpectPoseMatrix(const SampleRig& rig, const gfx::renderer::ModelState& state, std::string_view bone,
                      const PhysicsPose& pivot, float scale) {
    const auto sorted = rig.Sorted(bone);
    const auto& px = rig.baked->Bones().list[sorted].pivot;
    const auto rigid = FromPose(pivot);
    const auto expected = JPH::Mat44::sRotationTranslation(rigid.rotation, rigid.position) *
                          JPH::Mat44::sScale(JPH::Vec3::sReplicate(scale)) *
                          JPH::Mat44::sTranslation(JPH::Vec3(px[0], px[1], px[2]) * (-1.0f / 16));
    const auto& observed = state.PoseView().bone_poses[sorted].pose;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            EXPECT_NEAR(observed[c][r], expected(r, c), 1e-4f) << bone << " [" << c << "," << r << "]";
        }
    }
}

TEST(BoneSampleTest, ScaledNestedPivotsAndAbsoluteChannelsMatchRenderMatrices) {
    SampleRig rig({
        Bone("tip", "bridge", {13, -11, 5}, {-0.2f, 0.3f, 0.1f}), Bone("decor", "root"),
        Bone("pin", "root", {12, 18, 4}, {0.3f, -0.21f, 0.5f}),
        Bone("bridge", "parent", {10, -3, 2}, {0.1f, 0.25f, -0.2f}),
        Bone("parent", "root", {8, 3, 1}, {0.4f, -0.31f, 0.2f}),
        Bone("root", "", {1, 2, 3}, {0.15f, 0.05f, 0.3f})},
        {Ball("tip"), Ball("pin", NodeKind::kProxy), Ball("parent")},
        {WideTranslation("pin", "parent"), WideTranslation("parent", "tip")});
    rig.Channels("root").position[0] = 6;
    rig.Channels("pin").position[0] = 4;
    rig.Channels("tip").position[2] = -2;
    rig.Channels("decor").rotation[0] = std::numeric_limits<float>::quiet_NaN();
    rig.position = {3e7, -13, -3e7};
    rig.rotation = {0, 0.5f, 0, std::sqrt(0.75f)};
    ASSERT_NE(rig.Sorted("tip"), 0u);
    for (const float s : {0.5f, 2.0f}) {
        rig.Scale("root", s);
        rig.Scale("tip", 1.5f);
        ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
        gfx::renderer::ModelState state;
        ASSERT_TRUE(state.Extract(kGeneric, rig.baked, rig.channels, rig.channels.size()).ok());
        for (const auto bone : {"pin", "parent", "tip"}) {
            const auto& animation = rig.world.ReadAnimationTarget(rig.Slot(bone));
            ExpectPoseMatrix(rig, state, bone, animation.pivot, bone == std::string_view("tip") ? 1.5f * s : s);
        }

        rig.Place("parent", {-1.25f, 0.5f, 0.25f}, JPH::Quat::sEulerAngles({0.2f, 0.4f, 0.75f}));
        rig.Place("tip", {0.75f, -0.9f, 0.5f}, JPH::Quat::sEulerAngles({-0.8f, 0.15f, 0.5f}));
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        const auto corrections = rig.world.ActiveDeltas();
        ASSERT_EQ(corrections.size(), 2u);
        EXPECT_EQ(corrections[0].bone_index, rig.Sorted("parent"));
        EXPECT_EQ(corrections[1].bone_index, rig.Sorted("tip"));
        ASSERT_TRUE(state.Extract(kGeneric, rig.baked, rig.channels, rig.channels.size(), corrections).ok());
        ExpectPoseMatrix(rig, state, "parent", rig.world.ReadNodePivot(rig.Slot("parent")), s);
        ExpectPoseMatrix(rig, state, "tip", rig.world.ReadNodePivot(rig.Slot("tip")), 1.5f * s);
    }
}

TEST(BoneSampleTest, IllegalityCascadesAndRejoinsOnlyTheDetachedBodies) {
    auto first = WideTranslation("proxy", "piece");
    first.frame_a.pivot_offset = {4, 0, 0};
    SampleRig rig({Bone("proxy", "mount"), Bone("child", "proxy"), Bone("mount"), Bone("piece"),
                   Bone("healthy"), Bone("pin")},
        {Ball("piece"), Ball("proxy", NodeKind::kProxy), Ball("healthy"), Ball("child"), Ball("pin", NodeKind::kAnchor)},
        {first, WideTranslation("proxy", "child"), WideTranslation("pin", "healthy")});
    rig.Channels("healthy").position[1] = 64;
    ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
    rig.Body("healthy").SetLinearVelocity({1, 0, 0});
    const auto original = rig.Body("healthy").GetID();
    const auto previous = rig.Body("piece").GetID();
    rig.Channels("mount").position[1] = std::numeric_limits<float>::infinity();
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    rig.ExpectReason("proxy", Kind::kInvalidPose, rig.Sorted("mount"));
    rig.ExpectReason("child", Kind::kInvalidPose, rig.Sorted("mount"));
    rig.ExpectReason("piece", Kind::kLostAnchor, 0);
    EXPECT_EQ(rig.world.ReadContact(rig.Slot("piece"), rig.Slot("healthy")), ContactState::kIneligible);
    rig.Channels("mount").position[1] = 0;
    rig.Scale("mount", 0.25f);
    for (float s : {3e-6f, 5e6f, -1.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()}) {
        rig.Scale("proxy", s);
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        rig.ExpectReason("proxy", Kind::kInvalidScale, rig.Sorted("proxy"));
        rig.ExpectReason("child", Kind::kInvalidScale, rig.Sorted("proxy"));
        rig.ExpectReason("healthy", Kind::kActive, 0);
        const DebugSnapshot snapshot(rig.world);
        EXPECT_EQ(snapshot.ints[5 * DebugSnapshotOut::kNodeInts], 2);
        EXPECT_EQ(snapshot.floats[5 * DebugSnapshotOut::kNodeFloats + 14], 0);
        EXPECT_EQ(snapshot.floats[5 * DebugSnapshotOut::kNodeFloats + 21], -0.25f);
        EXPECT_EQ(rig.Body("healthy").GetID(), original);
        EXPECT_NEAR(rig.Body("healthy").GetLinearVelocity().GetX(), 1, 1e-5f);
    }
    rig.Scale("proxy", 1);
    rig.Channels("piece").position[0] = -32;
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    EXPECT_NE(rig.Body("piece").GetID(), previous);
    EXPECT_NEAR(rig.world.ReadNodePivot(rig.Slot("piece")).position[0], 2, 1e-5f);
    EXPECT_LT(rig.Body("piece").GetLinearVelocity().Length(), 1e-5f);
    EXPECT_GT(rig.world.ReadNodePivot(rig.Slot("healthy")).position[0], 0.08f);
    rig.Channels("pin").rotation[1] = std::numeric_limits<float>::quiet_NaN();
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    rig.ExpectReason("pin", Kind::kInvalidPose, rig.Sorted("pin"));
    EXPECT_FALSE(rig.world.ReadJointAnimationTarget(2));
    rig.ExpectReason("healthy", Kind::kLostAnchor, 2);
    EXPECT_TRUE(rig.world.IsNodeActive(rig.Slot("piece")));
}

TEST(BoneSampleTest, OnlyEffectiveEdgesAnchorTheWholeDetachedComponent) {
    for (const int empty_kind : {0, 1, 2}) {
        auto edge = Link(schema::PhysicsConstraintKind::kGeneric, "pin", "far");
        if (empty_kind == 1) {
            constexpr float pi = std::numbers::pi_v<float>;
            edge.angular_z = schema::AxisConfig{.limit = schema::LimitKind::kHard, .min = -pi, .max = pi};
        } else if (empty_kind == 2) {
            const schema::SpringParameters zero{schema::StiffnessDamping{0, 0}};
            edge.angular_x = schema::AxisConfig{.limit = schema::LimitKind::kSoft, .min = -1, .max = 1,
                                                .soft_limit_spring = zero};
            edge.linear_y = schema::AxisConfig{.spring = schema::SpringConfig{schema::SpringTarget::kAnimation, zero}};
        }
        auto bridge = Ball("bridge");
        bridge.disable_when_hidden = true;
        auto dead_end = Ball("dead-end");
        dead_end.disable_when_hidden = true;
        SampleRig rig({Bone("pin"), Bone("bridge"), Bone("near"), Bone("far"), Bone("dead-end")},
            {bridge, Ball("near"), Ball("far"), Ball("pin", NodeKind::kAnchor), dead_end},
            {WideTranslation("dead-end", "near"), WideTranslation("pin", "bridge"),
             WideTranslation("bridge", "near"), WideTranslation("near", "far"), edge});
        ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
        ASSERT_EQ(rig.world.ActiveDeltas().size(), 4u);
        rig.Channels("bridge").cubes_hidden = 1;
        rig.Channels("dead-end").cubes_hidden = 1;
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        rig.ExpectReason("bridge", Kind::kCubesHidden, rig.Sorted("bridge"));
        rig.ExpectReason("near", Kind::kLostAnchor, 2);
        rig.ExpectReason("far", Kind::kLostAnchor, 2);
        EXPECT_TRUE(rig.world.HasSample());
        EXPECT_TRUE(rig.world.ActiveDeltas().empty());
        rig.Channels("bridge").cubes_hidden = 0;
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        EXPECT_EQ(rig.world.ActiveDeltas().size(), 3u);
    }
}

TEST(BoneSampleTest, HiddenChannelsAndNonuniformChildKeepTheirDistinctInheritance) {
    auto parent = Ball("parent");
    auto child = Ball("child");
    parent.disable_when_hidden = child.disable_when_hidden = true;
    auto persistent = Ball("persistent");
    persistent.collection_mask = 0;
    auto root_bone = Bone("parent");
    root_bone.cubeCount = 1;
    schema::CubeLegacy outline{1, {0, 0, 0, -1, 0, 0, -1, 1, 0, 0, 1, 0},
                               {0, 1, 2, 3}, {0, 0, 1, 0, 1, 1, 0, 1}, {0, 1, 2, 3}, {0, 0, -1}};
    SampleRig rig({root_bone, Bone("child", "parent", {16, 0, 0}), Bone("locator", "child", {32, 0, 0}),
                   Bone("persistent", "parent", {0, 64, 0}), Bone("pin")},
        {parent, child, persistent, Ball("pin", NodeKind::kAnchor)},
        {WideTranslation("pin", "parent"), WideTranslation("pin", "child"), WideTranslation("pin", "persistent")},
        schema::Cubes{{outline}});
    rig.Channels("locator").locator_sequence = 1;
    rig.Channels("parent").transparency_glow = 0;
    ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
    ASSERT_EQ(rig.world.ActiveDeltas().size(), 3u);
    rig.Channels("parent").cubes_hidden = 1;
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    rig.ExpectReason("parent", Kind::kCubesHidden, rig.Sorted("parent"));
    rig.ExpectReason("child", Kind::kActive, 0);
    rig.Channels("parent").cubes_hidden = 0;
    rig.Channels("parent").children_hidden = 1;
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    rig.ExpectReason("parent", Kind::kActive, 0);
    rig.ExpectReason("child", Kind::kChildrenHidden, rig.Sorted("parent"));
    rig.ExpectReason("persistent", Kind::kActive, 0);
    rig.Channels("parent").children_hidden = 0;
    rig.Channels("child").scale[1] = 2;
    constexpr float angle = std::numbers::pi_v<float> / 6;
    rig.Place("parent", {0, 0, 0}, JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), angle));
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    rig.ExpectReason("child", Kind::kInvalidScale, rig.Sorted("child"));
    gfx::renderer::ModelState state;
    const auto rendered = state.Extract(kGeneric, rig.baked, rig.channels, rig.channels.size(), rig.world.ActiveDeltas());
    ASSERT_TRUE(rendered.ok()) << rendered.status();
    ASSERT_EQ(rendered->locator_count, 1u);
    EXPECT_EQ(state.StagedLocatorBoneIndices(1)[0], rig.Sorted("locator"));
    const auto& child_pose = state.PoseView().bone_poses[rig.Sorted("child")].pose;
    const auto& locator_pose = state.PoseView().bone_poses[rig.Sorted("locator")].pose;
    EXPECT_NEAR(child_pose[0][0], std::cos(angle), 1e-4f);
    EXPECT_NEAR(child_pose[0][1], std::sin(angle), 1e-4f);
    EXPECT_NEAR(locator_pose[0][0] * 2 + locator_pose[3][0], 2 * std::cos(angle), 1e-4f);
    EXPECT_NEAR(locator_pose[0][1] * 2 + locator_pose[3][1], 2 * std::sin(angle), 1e-4f);
}

TEST(BoneSampleTest, FullAngularSoftBackupCannotHoldABodyAfterItsAnchorScaleFails) {
    constexpr float pi = std::numbers::pi_v<float>;
    for (const auto axis : {&schema::PhysicsConstraint::angular_x, &schema::PhysicsConstraint::angular_y,
                           &schema::PhysicsConstraint::angular_z}) {
        auto backup = Link(schema::PhysicsConstraintKind::kGeneric, "backup", "body");
        backup.*axis = schema::AxisConfig{.limit = schema::LimitKind::kSoft, .min = -pi, .max = pi,
            .soft_limit_spring = schema::SpringParameters{schema::FrequencyDamping{2, 1}}};
        SampleRig rig({Bone("first"), Bone("backup"), Bone("body")},
            {Ball("body"), Ball("first", NodeKind::kAnchor), Ball("backup", NodeKind::kAnchor)},
            {WideTranslation("first", "body"), backup});
        ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
        rig.Body("body").SetLinearVelocity(JPH::Vec3(0, 3, 0));
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        EXPECT_NEAR(rig.Body("body").GetPosition().GetY(), 3 * kDt, 1e-5f);

        rig.Channels("first").scale[0] = 2;
        rig.Channels("body").position[1] = 12;
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        rig.ExpectReason("body", Kind::kLostAnchor, rig.Sorted("first"));
        EXPECT_TRUE(rig.world.IsNodeActive(rig.Slot("backup")));
        EXPECT_TRUE(rig.world.ActiveDeltas().empty());

        rig.Scale("first", 1);
        rig.Channels("body").position[1] = 20;
        ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
        ASSERT_TRUE(rig.world.IsNodeActive(rig.Slot("body")));
        EXPECT_NEAR(rig.Body("body").GetPosition().GetY(), 20.0f / 16, 1e-5f);
        EXPECT_NEAR(rig.Body("body").GetLinearVelocity().Length(), 0, 1e-5f);
    }
}

TEST(BoneSampleTest, PartialGatherAndOtherWorldCannotReplaceThePublishedSample) {
    const auto bones = std::vector{Bone("pin"), Bone("body", "", {16, 0, 0})};
    const auto nodes = std::vector{Ball("body"), Ball("pin", NodeKind::kProxy)};
    auto joint = WideTranslation("pin", "body");
    joint.frame_a.pivot_offset = {8, 0, 0};
    joint.frame_b.pivot_offset = {0, 16, 0};
    SampleRig first(bones, nodes, {joint}), other(bones, nodes, {joint});
    first.Scale("pin", 2);
    first.Scale("body", 0.5f);
    first.Channels("body").position[0] = -16;
    ASSERT_EQ(first.Advance(true).outcome, StepOutcome::kSuccess);
    const auto measured = first.world.ReadJointAnimationTarget(0);
    ASSERT_TRUE(measured);
    EXPECT_NEAR(measured->position[0], 1, 1e-5f);
    EXPECT_NEAR(measured->position[1], 0.5f, 1e-5f);
    first.Channels("body").position[0] = 48;
    ASSERT_TRUE(first.Gather());
    ASSERT_EQ(other.Advance(true).outcome, StepOutcome::kSuccess);
    EXPECT_EQ(first.world.ReadJointAnimationTarget(0).value().position, measured->position);
    EXPECT_EQ(first.world.ReadAnimationTarget(first.Slot("body")).scale, 0.5f);
    EXPECT_NE(other.world.ReadJointAnimationTarget(0).value().position, measured->position);
    ASSERT_EQ(first.world.ActiveDeltas().size(), 1u);
    // The step finished with finite bodies. Only conversion to the scaled local delta overflows.
    first.Channels("body").position[0] = std::numeric_limits<float>::max();
    first.Scale("body", 1.0e-6f);
    const auto failed = first.Advance(false);
    EXPECT_EQ(failed.outcome, StepOutcome::kNonFiniteState);
    EXPECT_EQ(failed.engine_error, 0u);
    EXPECT_FALSE(first.world.HasSample());
    EXPECT_TRUE(first.world.ActiveDeltas().empty());
    EXPECT_EQ(first.world.ReadContact(0, 1), ContactState::kNoSample);
    EXPECT_TRUE(other.world.HasSample());
    EXPECT_EQ(other.world.ActiveDeltas().size(), 1u);
    first.Channels("body").position[0] = 48;
    first.Scale("body", 1);
    ASSERT_EQ(first.Advance(true).outcome, StepOutcome::kSuccess);
    EXPECT_EQ(first.world.ActiveDeltas().size(), 1u);
}

TEST(BoneSampleTest, InvalidRootClearsLocalBodiesAndValidRootRejoinsAtCurrentPose) {
    SampleRig rig({Bone("pin"), Bone("body")}, {Ball("body"), Ball("pin", NodeKind::kAnchor)},
                  {WideTranslation("pin", "body")});
    rig.position = {-1e6, 2e6, 3e7};
    ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
    rig.position[2] = std::numeric_limits<double>::infinity();
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    EXPECT_TRUE(rig.world.HasSample());
    EXPECT_TRUE(rig.world.ActiveDeltas().empty());
    rig.ExpectReason("pin", Kind::kInvalidPose, rig.Sorted("pin"));
    rig.position[2] = 3e7;
    rig.Channels("body").position[0] = -24;
    ASSERT_EQ(rig.Advance(false).outcome, StepOutcome::kSuccess);
    EXPECT_NEAR(rig.world.ReadNodePivot(rig.Slot("body")).position[0], 1.5f, 1e-5f);
    EXPECT_EQ(rig.Body("body").GetLinearVelocity(), JPH::Vec3::sZero());
}

TEST(BoneSampleTest, FiniteModelChannelsCanOverflowAtSimRootForAnyNodeKind) {
    SampleRig rig({Bone("group"), Bone("body", "group"), Bone("proxy", "group"), Bone("pin", "group"),
                   Bone("healthy"), Bone("identity")},
        {Ball("body"), Ball("proxy", NodeKind::kProxy), Ball("pin", NodeKind::kAnchor),
         Ball("healthy"), Ball("identity", NodeKind::kAnchor)},
        {WideTranslation("pin", "body"), WideTranslation("identity", "healthy")});
    rig.Scale("group", 16);
    rig.rotation = {0, 0, std::sin(std::numbers::pi_v<float> / 8), std::cos(std::numbers::pi_v<float> / 8)};
    for (const auto node : {"body", "proxy", "pin"}) {
        auto& t = rig.Channels(node).position;
        t[0] = -2.7e38f;
        t[1] = 2.7e38f;
        ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
        rig.ExpectReason(node, Kind::kInvalidPose, rig.Sorted(node));
        rig.ExpectReason("healthy", Kind::kActive, 0);
        t[0] = t[1] = 0;
    }
}

TEST(BoneSampleTest, NonRepresentableDynamicInverseInertiaCascadesBeforeBodyCreation) {
    // The native arithmetic guard can be exercised independently of authoring limits.
    auto tiny = Ball("tiny", NodeKind::kDynamic, 1.0e-12f);
    SampleRig rig({Bone("tiny"), Bone("child", "tiny"), Bone("pin"), Bone("healthy")},
        {tiny, Ball("child"), Ball("pin", NodeKind::kAnchor), Ball("healthy")},
        {WideTranslation("pin", "tiny"), WideTranslation("pin", "child"), WideTranslation("pin", "healthy")});
    rig.Scale("tiny", 1.0e-6f);
    ASSERT_EQ(rig.Advance(true).outcome, StepOutcome::kSuccess);
    rig.ExpectReason("tiny", Kind::kInvalidInertia, rig.Sorted("tiny"));
    rig.ExpectReason("child", Kind::kInvalidInertia, rig.Sorted("tiny"));
    rig.ExpectReason("healthy", Kind::kActive, 0);
}
}  // namespace
}  // namespace ysm::physics
