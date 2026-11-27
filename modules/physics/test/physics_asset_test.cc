#include "../src/physics_asset_internal.h"

#include <cmath>
#include <limits>
#include <numbers>

#include <gtest/gtest.h>

#include "physics/runtime.h"
#include "proto/asset/model/data/geo_model.proto.h"

namespace ysm::physics {
class PhysicsAssetTest : public ::testing::Test {
protected:
    static const PhysicsAsset::Impl& Data(const PhysicsAsset& asset) { return asset.Pimpl(); }
};

namespace {
schema::CollisionShape Sphere(float radius, float x = 0, float y = 0, float z = 0) {
    return {schema::SphereShape{{x, y, z}, radius}};
}

schema::PhysicsNode Dynamic(std::string id, std::vector<schema::CollisionShape> shapes) {
    return {.id = id, .bone = id, .kind = schema::PhysicsNodeKind::kDynamic, .shapes = std::move(shapes)};
}

schema::PhysicsConstraint Joint(std::string a, std::string b, schema::PhysicsConstraintKind kind) {
    const schema::ConstraintFrame frame{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    return {.node_a = std::move(a), .node_b = std::move(b), .kind = kind, .frame_a = frame, .frame_b = frame};
}

auto Compile(std::vector<schema::PhysicsNode> nodes,
             std::vector<schema::PhysicsConstraint> constraints = {}) {
    InitializeRuntime();
    schema::GeoModel geo;
    for (const auto& node : nodes) {
        constraints.push_back(Joint("root", node.id, schema::PhysicsConstraintKind::kFixed));
    }
    nodes.push_back({.id = "root", .bone = "root", .kind = schema::PhysicsNodeKind::kAnchor});
    for (const auto& node : nodes) {
        auto& bone = geo.bones.emplace_back();
        bone.name = *node.bone;
        bone.pivot = {0, 0, 0};
        bone.rotate = {0, 0, 0};
    }
    for (size_t i = 0; i < constraints.size(); ++i) {
        constraints[i].id = std::to_string(i);
    }
    schema::PhysicsDefinition definition{.nodes = std::move(nodes), .constraints = std::move(constraints)};
    std::string geometry_bytes, physics_bytes;
    struct_pb::to_pb(geo, geometry_bytes);
    struct_pb::to_pb(definition, physics_bytes);
    return CompilePhysicsAsset(StrBuf(geometry_bytes), StrBuf(physics_bytes));
}

JPH::Vec3 ApplyInertiaInBone(const Node& node, JPH::Vec3Arg v) {
    const auto axes = node.shape_from_bone.rotation * node.base_inertia_rotation;
    return axes * ((axes.Conjugated() * v) / node.base_inv_inertia_diagonal);
}

void ExpectVector(JPH::Vec3Arg value, JPH::Vec3Arg expected, float tolerance) {
    EXPECT_NEAR(value.GetX(), expected.GetX(), tolerance);
    EXPECT_NEAR(value.GetY(), expected.GetY(), tolerance);
    EXPECT_NEAR(value.GetZ(), expected.GetZ(), tolerance);
}

TEST_F(PhysicsAssetTest, KeepsTinySphereInertiaAndBlenderDefaults) {
    auto small = Dynamic("small", {Sphere(0.25f, 8, 0, 0)});
    small.mass = 0.005f;
    const auto asset = Compile({small, Dynamic("default", {Sphere(1)})});
    ASSERT_TRUE(asset.ok()) << asset.status();
    const auto& nodes = Data(**asset).nodes;
    constexpr float moment = 0.4f * 0.005f * (0.25f / 16) * (0.25f / 16);
    ExpectVector(ApplyInertiaInBone(nodes[0], JPH::Vec3::sOne()), JPH::Vec3::sReplicate(moment),
                 moment * 1e-6f);
    ExpectVector(nodes[0].com_in_bone, JPH::Vec3(0.5f, 0, 0), 0);
    EXPECT_FLOAT_EQ(nodes[1].mass, 1.0f);
    EXPECT_FLOAT_EQ(nodes[1].linear_damping, 0.04f);
    EXPECT_FLOAT_EQ(nodes[1].angular_damping, 0.1f);
}

TEST_F(PhysicsAssetTest, RotatedSlenderBoxUsesShapeAxesExactlyOnce) {
    const double angle = std::numbers::pi / 8;
    schema::BoxShape box{{0, 0, 0},
                         {160, 16, 16},
                         {0, 0, static_cast<float>(std::sin(angle)), static_cast<float>(std::cos(angle))}};
    const auto asset = Compile({Dynamic("box", {{box}}), Dynamic("compound", {{box}, {box}})});
    ASSERT_TRUE(asset.ok()) << asset.status();
    const auto length_axis = JPH::Vec3(1, 1, 0).Normalized();
    const auto width_axis = JPH::Vec3(-1, 1, 0).Normalized();
    for (size_t i = 0; i < 2; ++i) {
        const auto& node = Data(**asset).nodes[i];
        ExpectVector(ApplyInertiaInBone(node, length_axis), length_axis / 6.0f, 3e-6f);
        ExpectVector(ApplyInertiaInBone(node, width_axis), width_axis * (101.0f / 12), 3e-6f);
    }
}

TEST_F(PhysicsAssetTest, WeightsUnequalSpheresByFullVolumesAtBothSeparations) {
    const auto direction = JPH::Vec3(2, -3, 6) / 7.0f;
    for (const float distance : {5.0f, 1.5f}) {
        SCOPED_TRACE(distance);
        const auto offset = direction * (16 * distance);
        auto node = Dynamic("pair", {Sphere(16), Sphere(32, offset.GetX(), offset.GetY(), offset.GetZ())});
        node.mass = 9.0f;
        const auto asset = Compile({node});
        ASSERT_TRUE(asset.ok()) << asset.status();
        const auto& compiled = Data(**asset).nodes.front();
        ExpectVector(compiled.com_in_bone, direction * (distance * 8 / 9), 1e-6f);
        // One kg in the first sphere and eight in the second. Translation adds only transverse inertia.
        constexpr float axial = 13.2f;
        const float radial = axial + 8.0f / 9 * distance * distance;
        for (const auto axis : {JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY(), JPH::Vec3::sAxisZ()}) {
            const auto expected = radial * axis + (axial - radial) * axis.Dot(direction) * direction;
            ExpectVector(ApplyInertiaInBone(compiled, axis), expected, 1e-5f);
        }
    }
}

TEST_F(PhysicsAssetTest, CapsuleHasAnalyticCapMomentsAndTinySeparationBecomesSphere) {
    schema::CapsuleShape capsule{{-16, 0, 0}, {16, 0, 0}, 16};
    schema::CapsuleShape tiny{{0, 0, 0}, {std::numeric_limits<float>::denorm_min(), 0, 0}, 16};
    const auto asset = Compile({Dynamic("capsule", {{capsule}}), Dynamic("tiny", {{tiny}})});
    ASSERT_TRUE(asset.ok()) << asset.status();
    const auto& nodes = Data(**asset).nodes;
    // The cylinder contains 3/5 of the mass, the two hemispheres together contain 2/5.
    ExpectVector(ApplyInertiaInBone(nodes[0], JPH::Vec3::sOne()), JPH::Vec3(0.46f, 1.21f, 1.21f), 1e-6f);
    EXPECT_EQ(nodes[1].shape->GetSubType(), JPH::EShapeSubType::Sphere);
    ExpectVector(ApplyInertiaInBone(nodes[1], JPH::Vec3::sOne()), JPH::Vec3::sReplicate(0.4f), 1e-6f);
}

TEST_F(PhysicsAssetTest, EnforcesPerNodeAndWholeAssetShapeBudgets) {
    std::vector<schema::PhysicsNode> nodes;
    for (int i = 0; i < 16; ++i) {
        nodes.push_back(Dynamic(std::to_string(i), std::vector<schema::CollisionShape>(32, Sphere(1))));
    }
    ASSERT_TRUE(Compile(nodes).ok());
    nodes.push_back(Dynamic("overflow", {Sphere(1)}));
    EXPECT_EQ(Compile(nodes).status().code(), absl::StatusCode::kResourceExhausted);
    nodes.resize(1);
    nodes[0].shapes.push_back(Sphere(1));
    EXPECT_EQ(Compile(nodes).status().code(), absl::StatusCode::kResourceExhausted);
}

TEST_F(PhysicsAssetTest, RejectsUnrepresentableDynamicMassAndMoments) {
    auto node = Dynamic("ball", {Sphere(1)});
    for (float mass : {0.0f, -1.0f, std::numeric_limits<float>::infinity()}) {
        node.mass = mass;
        EXPECT_EQ(Compile({node}).status().code(), absl::StatusCode::kInvalidArgument);
    }
    node.mass = 0.005f;
    node.shapes = {Sphere(1e-18f)};
    EXPECT_EQ(Compile({node}).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(PhysicsAssetTest, MechanicalEdgesMatchAdmittedAxisSemantics) {
    constexpr float pi = std::numbers::pi_v<float>;
    const auto generic = Joint("a", "b", schema::PhysicsConstraintKind::kGeneric);
    const schema::AxisConfig full_turn{.limit = schema::LimitKind::kHard, .min = -pi, .max = pi};
    const schema::SpringParameters zero{schema::StiffnessDamping{0, 0}};
    const schema::SpringParameters damper{schema::StiffnessDamping{0, 1}};
    const schema::AxisConfig soft_zero{
        .limit = schema::LimitKind::kSoft, .min = -1, .max = 1, .soft_limit_spring = zero};
    std::vector<schema::PhysicsConstraint> cases(9, generic);
    cases[1].angular_x = full_turn;
    cases[2].linear_y = soft_zero;
    cases[2].angular_z = {.spring = schema::SpringConfig{schema::SpringTarget::kRestPose, zero}};
    cases[3].angular_x = full_turn;
    cases[3].angular_x->max = std::nextafter(pi, 0.0f);
    cases[4].linear_x = full_turn;
    cases[5].linear_z = soft_zero;
    cases[5].linear_z->soft_limit_spring = damper;
    cases[6].angular_y = {.spring = schema::SpringConfig{schema::SpringTarget::kAnimation, damper}};
    cases[7].kind = schema::PhysicsConstraintKind::kHinge;
    cases[8].kind = schema::PhysicsConstraintKind::kPoint;
    const auto asset = Compile({Dynamic("a", {Sphere(1)}), Dynamic("b", {Sphere(1)})}, cases);
    ASSERT_TRUE(asset.ok()) << asset.status();
    const auto& joints = Data(**asset).constraints;
    for (size_t i = 0; i < joints.size(); ++i) {
        EXPECT_EQ(joints[i].is_effective_mechanical, i >= 3) << i;
    }
}

TEST_F(PhysicsAssetTest, SharedFilterIntersectsCollectionsAndRetainsEveryExcludedPair) {
    constexpr uint32_t last_collection = 1u << 19;
    std::vector<schema::PhysicsNode> nodes;
    for (int i = 0; i < 68; ++i) {
        nodes.push_back(Dynamic(std::to_string(i), {Sphere(1)}));
        nodes.back().collection_mask = last_collection;
    }
    nodes[0].collection_mask.reset();
    nodes[1].collection_mask = 0;
    nodes[2].collection_mask = last_collection | 1;
    auto empty = Joint("2", "64", schema::PhysicsConstraintKind::kGeneric);
    auto full = Joint("2", "65", schema::PhysicsConstraintKind::kGeneric);
    full.angular_x = {.limit = schema::LimitKind::kHard,
                      .min = -std::numbers::pi_v<float>,
                      .max = std::numbers::pi_v<float>};
    auto zero = Joint("2", "66", schema::PhysicsConstraintKind::kGeneric);
    zero.linear_x = {.limit = schema::LimitKind::kSoft,
                     .min = -1,
                     .max = 1,
                     .soft_limit_spring = schema::SpringParameters{schema::StiffnessDamping{0, 0}}};
    auto permit = empty;
    permit.disable_collisions = false;
    auto asset = Compile(nodes, {empty, full, zero, permit});
    ASSERT_TRUE(asset.ok()) << asset.status();
    auto group = [&](uint32_t i) {
        const auto& data = Data(**asset);
        return JPH::CollisionGroup(data.group_filter.GetPtr(), data.nodes[i].collection_mask, i);
    };
    const auto first = group(2);
    EXPECT_TRUE(first.CanCollide(group(0)));
    EXPECT_FALSE(group(0).CanCollide(group(3)));
    EXPECT_FALSE(first.CanCollide(group(1)));
    for (uint32_t i = 64; i <= 66; ++i) {
        EXPECT_FALSE(Data(**asset).constraints[i - 64].is_effective_mechanical);
        EXPECT_FALSE(first.CanCollide(group(i)));
        EXPECT_FALSE(group(i).CanCollide(first));
    }
    const auto permitted = group(67);
    asset->reset();
    EXPECT_TRUE(first.CanCollide(permitted));
}

TEST_F(PhysicsAssetTest, FullAngularLimitsAreEmptyUnlessAnIndependentSpringActs) {
    constexpr float pi = std::numbers::pi_v<float>;
    const auto generic = Joint("a", "b", schema::PhysicsConstraintKind::kGeneric);
    const schema::SpringParameters frequency{schema::FrequencyDamping{2, 1}};
    const schema::SpringParameters stiffness{schema::StiffnessDamping{12, 1}};
    for (const auto axis : {&schema::PhysicsConstraint::angular_x, &schema::PhysicsConstraint::angular_y,
                           &schema::PhysicsConstraint::angular_z}) {
        auto hard = generic;
        hard.*axis = schema::AxisConfig{.limit = schema::LimitKind::kHard, .min = -pi, .max = pi};
        for (const auto& spring : {frequency, stiffness}) {
            auto soft = generic;
            soft.*axis = schema::AxisConfig{.limit = schema::LimitKind::kSoft, .min = -pi, .max = pi,
                                           .soft_limit_spring = spring};
            auto bounded = soft;
            (bounded.*axis)->min = -pi / 2;
            (bounded.*axis)->max = pi / 2;
            auto driven = soft;
            (driven.*axis)->spring = schema::SpringConfig{schema::SpringTarget::kRestPose, spring};
            const auto asset = Compile({Dynamic("a", {Sphere(1)}), Dynamic("b", {Sphere(1)})},
                                       {hard, soft, bounded, driven});
            ASSERT_TRUE(asset.ok()) << asset.status();
            const auto& joints = Data(**asset).constraints;
            EXPECT_FALSE(joints[0].is_effective_mechanical);
            EXPECT_FALSE(joints[1].is_effective_mechanical);
            EXPECT_TRUE(joints[2].is_effective_mechanical);
            EXPECT_TRUE(joints[3].is_effective_mechanical);
        }
    }
}
}  // namespace
}  // namespace ysm::physics
