#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <ylt/struct_pb.hpp>

#include "fixture_directory.h"
#include "physics/physics_asset.h"
#include "physics/runtime.h"
#include "proto/asset/model/ModelData.proto.h"

namespace ysm::test {
namespace {

namespace model_proto = proto::asset::model;
namespace data = model_proto::data;

std::string ReadWireFixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << path;
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

TEST(PhysicsProtoTest, CompilesAdmittedPhysicsFromJavaModelData) {
    const auto& fixture_dir = FixtureDirectory();
    if (fixture_dir.empty()) {
        GTEST_SKIP() << "Pass --fixture-dir to locate the Java writer fixture";
    }
    model_proto::ModelData model;
    struct_pb::from_pb(model, ReadWireFixture(fixture_dir / "physics-model.pb"));
    ASSERT_EQ(model.geo_models.size(), 1u);
    ASSERT_TRUE(model.physics.has_value());
    const auto& geometry = model.geo_models.front().value;
    std::string geometry_bytes;
    std::string physics_bytes;
    struct_pb::to_pb(geometry, geometry_bytes);
    struct_pb::to_pb(*model.physics, physics_bytes);

    physics::InitializeRuntime();
    const auto compiled = physics::CompilePhysicsAsset(StrBuf(geometry_bytes), StrBuf(physics_bytes));
    ASSERT_TRUE(compiled.ok()) << compiled.status();
    EXPECT_NE(*compiled, nullptr);

    auto bad_shape = *model.physics;
    auto& sphere = std::get<data::SphereShape>(bad_shape.nodes.front().shapes.front().shape);
    // This finite positive input passes P4, but converting it to meters underflows.
    sphere.radius = std::numeric_limits<float>::denorm_min();
    physics_bytes.clear();
    struct_pb::to_pb(bad_shape, physics_bytes);
    const auto result = physics::CompilePhysicsAsset(StrBuf(geometry_bytes), StrBuf(physics_bytes));
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(PhysicsProtoTest, DecodesJavaProductionWriterBytes) {
    const auto& fixture_dir = FixtureDirectory();
    if (fixture_dir.empty()) {
        GTEST_SKIP() << "Pass --fixture-dir to locate the Java writer fixture";
    }
    const auto bytes = ReadWireFixture(fixture_dir / "physics-model.pb");
    ASSERT_FALSE(bytes.empty());

    model_proto::ModelData model;
    ASSERT_NO_THROW(struct_pb::from_pb(model, bytes));
    ASSERT_EQ(model.geo_models.size(), 1u);
    EXPECT_EQ(model.geo_models.front().key, "main");
    ASSERT_EQ(model.geo_models.front().value.bones.size(), 2u);
    EXPECT_EQ(model.geo_models.front().value.bones[1].name, "hair");
    ASSERT_TRUE(model.physics.has_value());
    const auto& physics = *model.physics;
    EXPECT_EQ(physics.gravity, (std::vector<float>{0.5F, -9.25F, -0.75F}));
    ASSERT_EQ(physics.collections.size(), 1u);
    EXPECT_EQ(physics.collections[0].slot, 20u);
    EXPECT_EQ(physics.collections[0].name, "hair");

    ASSERT_EQ(physics.nodes.size(), 4u);
    const auto& strand = physics.nodes[0];
    EXPECT_EQ(strand.id, "strand");
    EXPECT_EQ(strand.bone, "hair");
    EXPECT_EQ(strand.kind, data::PhysicsNodeKind::kDynamic);
    EXPECT_EQ(strand.collection_mask, 0u);
    EXPECT_EQ(strand.friction, 0.0F);
    EXPECT_EQ(strand.bounciness, 0.25F);
    EXPECT_EQ(strand.mass, 2.5F);
    EXPECT_EQ(strand.linear_damping, 0.125F);
    EXPECT_EQ(strand.angular_damping, 0.625F);
    EXPECT_EQ(strand.gravity_scale, -0.75F);
    EXPECT_EQ(strand.disable_when_hidden, true);
    EXPECT_EQ(strand.preset_tag, "hair_chain:strand");

    ASSERT_EQ(strand.shapes.size(), 3u);
    const auto* sphere = std::get_if<data::SphereShape>(&strand.shapes[0].shape);
    ASSERT_NE(sphere, nullptr);
    EXPECT_EQ(sphere->center, (std::vector<float>{1, -2, 3}));
    EXPECT_EQ(sphere->radius, 2.5F);
    const auto* capsule = std::get_if<data::CapsuleShape>(&strand.shapes[1].shape);
    ASSERT_NE(capsule, nullptr);
    EXPECT_EQ(capsule->point_a, (std::vector<float>{-1, 2, 3}));
    EXPECT_EQ(capsule->point_b, (std::vector<float>{4, 5, 6}));
    EXPECT_EQ(capsule->radius, 0.75F);
    const auto* box = std::get_if<data::BoxShape>(&strand.shapes[2].shape);
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(box->center, (std::vector<float>{0, -1, 2}));
    EXPECT_EQ(box->size, (std::vector<float>{2, 4, 6}));
    EXPECT_EQ(box->rotation, (std::vector<float>{0, 0.6F, 0, 0.8F}));

    const auto& anchor = physics.nodes[1];
    EXPECT_EQ(anchor.kind, data::PhysicsNodeKind::kAnchor);
    EXPECT_TRUE(anchor.shapes.empty());
    EXPECT_FALSE(anchor.collection_mask.has_value());
    const auto& proxy = physics.nodes[2];
    EXPECT_EQ(proxy.kind, data::PhysicsNodeKind::kProxy);
    EXPECT_EQ(proxy.collection_mask, 1u);
    EXPECT_FALSE(proxy.mass.has_value());
    EXPECT_FALSE(physics.nodes[3].collection_mask.has_value());

    ASSERT_EQ(physics.constraints.size(), 4u);
    const auto& generic = physics.constraints[0];
    EXPECT_EQ(generic.id, "lat");
    EXPECT_EQ(generic.node_a, "anchor");
    EXPECT_EQ(generic.node_b, "strand");
    EXPECT_EQ(generic.kind, data::PhysicsConstraintKind::kGeneric);
    EXPECT_EQ(generic.disable_collisions, false);
    EXPECT_EQ(generic.frame_a.pivot_offset, (std::vector<float>{0, 0, 0}));
    EXPECT_EQ(generic.frame_a.axis_x, (std::vector<float>{1, 0, 0}));
    EXPECT_EQ(generic.frame_a.axis_y, (std::vector<float>{0, 1, 0}));
    EXPECT_EQ(generic.frame_a.axis_z, (std::vector<float>{0, 0, 1}));
    EXPECT_EQ(generic.frame_b.pivot_offset, (std::vector<float>{-2, 8, 0.5F}));
    EXPECT_EQ(generic.frame_b.axis_x, (std::vector<float>{0, 1, 0}));
    EXPECT_EQ(generic.frame_b.axis_y, (std::vector<float>{-1, 0, 0}));
    EXPECT_EQ(generic.frame_b.axis_z, (std::vector<float>{0, 0, 1}));
    EXPECT_EQ(generic.preset_tag, "lateral_link");

    ASSERT_TRUE(generic.linear_x.has_value());
    const auto& linear_x = *generic.linear_x;
    EXPECT_EQ(linear_x.limit, data::LimitKind::kSoft);
    EXPECT_EQ(linear_x.min, 0.0F);
    EXPECT_EQ(linear_x.max, 1.25F);
    ASSERT_TRUE(linear_x.spring.has_value());
    EXPECT_EQ(linear_x.spring->target, data::SpringTarget::kRestPose);
    ASSERT_TRUE(linear_x.spring->parameters.has_value());
    const auto* frequency = std::get_if<data::FrequencyDamping>(&linear_x.spring->parameters->mode);
    ASSERT_NE(frequency, nullptr);
    EXPECT_EQ(frequency->frequency, 2.5F);
    EXPECT_EQ(frequency->damping_ratio, 0.0F);
    ASSERT_TRUE(linear_x.soft_limit_spring.has_value());
    const auto* stiffness = std::get_if<data::StiffnessDamping>(&linear_x.soft_limit_spring->mode);
    ASSERT_NE(stiffness, nullptr);
    EXPECT_EQ(stiffness->stiffness, 0.0F);
    EXPECT_EQ(stiffness->damping, 8.0F);

    ASSERT_TRUE(generic.linear_y.has_value());
    EXPECT_EQ(generic.linear_y->limit, data::LimitKind::kLocked);
    EXPECT_FALSE(generic.linear_y->min.has_value());
    ASSERT_TRUE(generic.linear_z.has_value());
    EXPECT_EQ(generic.linear_z->limit, data::LimitKind::kHard);
    EXPECT_EQ(generic.linear_z->min, -4.0F);
    EXPECT_EQ(generic.linear_z->max, 2.0F);
    ASSERT_TRUE(generic.angular_x.has_value());
    EXPECT_EQ(generic.angular_x->min, -std::numbers::pi_v<float>);
    EXPECT_EQ(generic.angular_x->max, std::numbers::pi_v<float>);
    ASSERT_TRUE(generic.angular_y.has_value() && generic.angular_y->spring.has_value());
    EXPECT_FALSE(generic.angular_y->limit.has_value());
    EXPECT_EQ(generic.angular_y->spring->target, data::SpringTarget::kAnimation);
    ASSERT_TRUE(generic.angular_z.has_value() && generic.angular_z->spring.has_value());
    EXPECT_EQ(generic.angular_z->limit, data::LimitKind::kFree);
    EXPECT_EQ(generic.angular_z->spring->target, data::SpringTarget::kOff);
    EXPECT_FALSE(generic.angular_z->spring->parameters.has_value());

    const auto& point = physics.constraints[1];
    EXPECT_EQ(point.kind, data::PhysicsConstraintKind::kPoint);
    EXPECT_FALSE(point.disable_collisions.has_value());
    ASSERT_TRUE(point.cone.has_value());
    EXPECT_EQ(point.cone->limit, data::LimitKind::kSoft);
    EXPECT_EQ(point.cone->half_angle, 0.75F);
    ASSERT_TRUE(point.cone->soft_limit_spring.has_value());
    EXPECT_TRUE(std::holds_alternative<data::FrequencyDamping>(point.cone->soft_limit_spring->mode));
    const auto& hinge = physics.constraints[2];
    EXPECT_EQ(hinge.kind, data::PhysicsConstraintKind::kHinge);
    EXPECT_EQ(hinge.disable_collisions, true);
    ASSERT_TRUE(hinge.angular_z.has_value());
    EXPECT_EQ(hinge.angular_z->min, 0.25F);
    EXPECT_EQ(hinge.angular_z->max, 0.5F);
    const auto& fixed = physics.constraints[3];
    EXPECT_EQ(fixed.kind, data::PhysicsConstraintKind::kFixed);
    EXPECT_FALSE(fixed.angular_z.has_value());
    EXPECT_FALSE(fixed.preset_tag.has_value());
}

TEST(PhysicsProtoTest, ReadsAndWritesModelWithoutPhysicsWithNoAddedPayload) {
    const auto& fixture_dir = FixtureDirectory();
    if (fixture_dir.empty()) {
        GTEST_SKIP() << "Pass --fixture-dir to locate the Java writer fixture";
    }
    const auto bytes = ReadWireFixture(fixture_dir / "physics-model-absent.pb");
    ASSERT_FALSE(bytes.empty());

    model_proto::ModelData model;
    ASSERT_NO_THROW(struct_pb::from_pb(model, bytes));
    EXPECT_FALSE(model.physics.has_value());
    ASSERT_EQ(model.geo_models.size(), 1u);
    EXPECT_EQ(model.geo_models.front().key, "main");
    EXPECT_EQ(model.geo_models.front().value.bones.size(), 2u);

    std::string emitted;
    struct_pb::to_pb(model_proto::ModelData{}, emitted);
    EXPECT_TRUE(emitted.empty());
}

}  // namespace
}  // namespace ysm::test
