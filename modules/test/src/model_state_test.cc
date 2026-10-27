#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <future>
#include <memory>
#include <numbers>
#include <ranges>
#include <vector>

#include <gtest/gtest.h>

#include "buffer.h"
#include "cpu.h"
#include "gfx/bake/baked_model.h"
#include "gfx/bake/baked_serializer.h"
#include "gfx/renderer/model_state.h"
#include "gfx/renderer/render.h"
#include "renderer/vertex/iris_56.h"
#include "renderer/vertex/vanilla.h"

namespace ysm::test {
namespace {
using Group = gfx::bake::CubeGroup<simd::Width::B128, false>;
constexpr simd::Tag<simd::Type::kNone> kGenericTag;

Group MakeQuadGroup(uint32_t bone_index) {
    Group group;
    group.bone_index = bone_index;
    group.cube_count = 1;
    group.cube_attr[0].quad_count = 1;
    group.cube_attr[0].quad_count_after_culling = 1;
    group.normal[1][0] = 1.0f;
    group.winding_sign[0] = 1.0f;
    const std::array<std::array<float, 3>, 4> positions{{
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
    }};
    for (size_t vertex = 0; vertex < positions.size(); ++vertex) {
        for (size_t axis = 0; axis < 3; ++axis) {
            group.pos[axis][vertex] = positions[vertex][axis];
        }
        group.vertex_index[vertex][0] = static_cast<uint32_t>(vertex);
    }
    return group;
}

std::shared_ptr<gfx::bake::BakedModel> MakeModel() {
    gfx::bake::BakedModelCubes<simd::Width::B128> cubes;
    cubes.cutout_no_culling = {MakeQuadGroup(1), MakeQuadGroup(3)};

    gfx::bake::BakedModelBones bones;
    bones.list.resize(4);
    bones.list[0].parent_index = UINT32_MAX;
    bones.list[0].subtree_end = 4;
    bones.list[0].depth = 0;
    bones.list[1].parent_index = 0;
    bones.list[1].subtree_end = 3;
    bones.list[1].depth = 1;
    bones.list[1].pivot = {16, 0, 0};
    bones.list[2].parent_index = 1;
    bones.list[2].subtree_end = 3;
    bones.list[2].depth = 2;
    bones.list[2].pivot = {0, 16, 0};
    bones.list[3].parent_index = 0;
    bones.list[3].subtree_end = 4;
    bones.list[3].depth = 1;
    for (auto& bone : bones.list) {
        bone.solid = true;
    }

    bones.cube_indices_cache = {0, 1};
    bones.cube_group_info_cache = {{1, 1, 1}, {1, 1, 1}};
    auto& first = bones.list[1].cutout_no_culling;
    first.cube_indices = std::span(bones.cube_indices_cache).subspan(0, 1);
    first.cube_group_info =
        std::span(bones.cube_group_info_cache).subspan(0, 1);
    first.cube_count = 1;
    first.full_vertex_count = 4;
    first.culling_vertex_count = 4;
    auto& second = bones.list[3].cutout_no_culling;
    second.cube_indices = std::span(bones.cube_indices_cache).subspan(1, 1);
    second.cube_group_info =
        std::span(bones.cube_group_info_cache).subspan(1, 1);
    second.cube_count = 1;
    second.full_vertex_count = 4;
    second.culling_vertex_count = 4;
    return std::make_shared<gfx::bake::BakedModel>(gfx::bake::BakedModelInfo{},
                                              std::move(bones),
                                              std::move(cubes));
}

std::shared_ptr<gfx::bake::BakedModel> MakeFlatModel(size_t bone_count) {
    gfx::bake::BakedModelCubes<simd::Width::B128> cubes;
    cubes.cutout_no_culling.reserve(bone_count);
    for (size_t bone_index = 0; bone_index < bone_count; ++bone_index) {
        cubes.cutout_no_culling.push_back(
            MakeQuadGroup(static_cast<uint32_t>(bone_index)));
    }

    gfx::bake::BakedModelBones bones;
    bones.list.resize(bone_count);
    bones.sorted_bone_indices.reserve(bone_count);
    bones.cube_indices_cache.resize(bone_count);
    bones.cube_group_info_cache.resize(bone_count);
    for (size_t bone_index = 0; bone_index < bone_count; ++bone_index) {
        bones.sorted_bone_indices.push_back(
            static_cast<uint16_t>(bone_index));
        bones.cube_indices_cache[bone_index] =
            static_cast<uint32_t>(bone_index);
        bones.cube_group_info_cache[bone_index] = {1, 1, 1};

        auto& bone = bones.list[bone_index];
        bone.parent_index = UINT32_MAX;
        bone.subtree_end = static_cast<uint32_t>(bone_index + 1);
        bone.depth = 0;
        bone.solid = true;
        auto& partition = bone.cutout_no_culling;
        partition.cube_indices =
            std::span(bones.cube_indices_cache).subspan(bone_index, 1);
        partition.cube_group_info =
            std::span(bones.cube_group_info_cache).subspan(bone_index, 1);
        partition.cube_count = 1;
        partition.full_vertex_count = 4;
        partition.culling_vertex_count = 4;
    }
    return std::make_shared<gfx::bake::BakedModel>(gfx::bake::BakedModelInfo{},
                                              std::move(bones),
                                              std::move(cubes));
}

std::vector<gfx::renderer::BoneAttribute> MakeAttributes() {
    std::vector<gfx::renderer::BoneAttribute> attributes(4);
    for (auto& attribute : attributes) {
        attribute.scale[0] = 1.0f;
        attribute.scale[1] = 1.0f;
        attribute.scale[2] = 1.0f;
    }
    attributes[2].locator_sequence = 5.0f;
    return attributes;
}

std::vector<gfx::renderer::BoneAttribute> MakeFlatAttributes(size_t bone_count) {
    std::vector<gfx::renderer::BoneAttribute> attributes(bone_count);
    for (size_t bone_index = 0; bone_index < bone_count; ++bone_index) {
        auto& attribute = attributes[bone_index];
        attribute.position[0] = static_cast<float>(bone_index) * -16.0f;
        attribute.scale[0] = 1.0f;
        attribute.scale[1] = 1.0f;
        attribute.scale[2] = 1.0f;
    }
    return attributes;
}
}  // namespace

TEST(ModelStateTest, ConcurrentSerializationBorrowsOnlyImmutableGeometry) {
    InitCpuInfo();
    auto model = MakeFlatModel(4);
    auto attributes = MakeFlatAttributes(4);
    auto serializing = std::async(std::launch::async, [model] {
        for (int iteration = 0; iteration < 100; ++iteration) {
            auto bytes = gfx::bake::SerializeBakedModel(*model);
            if (bytes.size() != gfx::bake::BakedModelSerializationBytes(*model))
                return false;
        }
        return true;
    });
    gfx::renderer::ModelState state;
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto result =
            state.Extract(kGenericTag, model, attributes, attributes.size());
        ASSERT_TRUE(result.ok()) << result.status();
        EXPECT_EQ(result->vertex_count, 16);
    }
    EXPECT_TRUE(serializing.get());
}

TEST(ModelStateTest, ExtractsPoseLocatorsAndReusesSchedule) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    attributes[0].position[0] = 16.0f;
    attributes[1].rotation[2] = std::numbers::pi_v<float> * 0.5f;
    attributes[1].scale[0] = 2.0f;
    gfx::renderer::ModelState state;

    auto first =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(first.ok()) << first.status();
    EXPECT_TRUE(first->schedule_updated);
    EXPECT_EQ(first->vertex_count, 8);
    EXPECT_EQ(first->locator_count, 1);
    EXPECT_EQ(state.StagedLocatorBoneIndices(first->locator_count)[0], 2);
    EXPECT_TRUE(state.IsValid());
    EXPECT_TRUE(std::ranges::equal(state.PoseView().render_bone_indices,
                                   std::array<uint16_t, 2>{1, 3}));

    const auto& child = state.PoseView().bone_poses[1];
    EXPECT_NEAR(child.pose[0][0], 0.0f, 0.00001f);
    EXPECT_NEAR(child.pose[0][1], 2.0f, 0.00001f);
    EXPECT_NEAR(child.pose[1][0], -1.0f, 0.00001f);
    EXPECT_NEAR(child.pose[3][0], 0.0f, 0.00001f);
    EXPECT_NEAR(child.pose[3][1], -2.0f, 0.00001f);
    EXPECT_NEAR(child.normal[0][1], 0.5f, 0.00001f);
    EXPECT_NEAR(child.normal[1][0], -1.0f, 0.00001f);
    const auto& sibling = state.PoseView().bone_poses[3];
    EXPECT_FLOAT_EQ(sibling.pose[0][0], 1.0f);
    EXPECT_FLOAT_EQ(sibling.pose[0][1], 0.0f);
    EXPECT_FLOAT_EQ(sibling.pose[3][0], -1.0f);
    EXPECT_FLOAT_EQ(sibling.normal[0][0], 1.0f);
    EXPECT_FLOAT_EQ(sibling.normal[0][1], 0.0f);

    attributes[1].position[0] = 4.0f;
    auto second =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(second.ok()) << second.status();
    EXPECT_FALSE(second->schedule_updated);
    EXPECT_EQ(second->vertex_count, 8);
    EXPECT_EQ(second->locator_count, 1);

    auto other_model = MakeModel();
    auto switched =
        state.Extract(kGenericTag, other_model, attributes, attributes.size());
    ASSERT_TRUE(switched.ok()) << switched.status();
    EXPECT_TRUE(switched->schedule_updated);
    EXPECT_EQ(state.Model(), other_model);
}

TEST(ModelStateTest, LocalRootIgnoresAllExternalAncestorAttributes) {
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    attributes[1].rotation[2] = std::numbers::pi_v<float> * 0.5f;
    attributes[1].scale[0] = 2.0f;
    gfx::renderer::ModelState state;
    for (const auto ancestor : std::array<gfx::renderer::BoneAttribute, 6>{
             attributes[0],
             {.position = {16, 32, 48}},
             {.rotation = {0.3f, 0.6f, 0.9f}},
             {.scale = {2, 3, 4}},
             {.scale = {0, 0, 0}},
             {.children_hidden = 1}}) {
        attributes[0] = ancestor;
        // Invalid packed data outside the range must not be read either.
        attributes[0].color = -1;
        auto result = state.ExtractSubtree(kGenericTag, model, attributes, 4, 1);
        ASSERT_TRUE(result.ok()) << result.status();
        EXPECT_EQ(result->vertex_count, 4);
        EXPECT_EQ(result->locator_count, 1);
        EXPECT_EQ(state.StagedLocatorBoneIndices(1)[0], 2);
        EXPECT_TRUE(std::ranges::equal(state.PoseView().render_bone_indices,
                                       std::array<uint16_t, 1>{1}));
        const auto poses = state.PoseView().bone_poses;
        EXPECT_NEAR(poses[1].pose[0][1], 2, 0.00001f);
        EXPECT_NEAR(poses[1].pose[1][0], -1, 0.00001f);
        EXPECT_NEAR(poses[1].pose[3][0], 1, 0.00001f);
        EXPECT_NEAR(poses[1].pose[3][1], -2, 0.00001f);
        EXPECT_NEAR(poses[1].normal[0][1], 0.5f, 0.00001f);
        EXPECT_NEAR(poses[1].normal[1][0], -1, 0.00001f);
        EXPECT_NEAR(poses[2].pose[0][1], 2, 0.00001f);
    }
}

TEST(ModelStateTest, LocalRangesRebuildEqualSizedSchedulesAndRecoverAfterFailure) {
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    gfx::renderer::ModelState state;
    for (const size_t root : {1u, 3u, 1u}) {
        auto result = state.ExtractSubtree(kGenericTag, model, attributes, 4, root);
        ASSERT_TRUE(result.ok()) << result.status();
        EXPECT_TRUE(result->schedule_updated);
        EXPECT_EQ(result->vertex_count, 4);
        EXPECT_EQ(state.PoseView().render_bone_indices[0], root);
    }
    auto reused = state.ExtractSubtree(kGenericTag, model, attributes, 4, 1);
    ASSERT_TRUE(reused.ok());
    EXPECT_FALSE(reused->schedule_updated);
    for (const size_t root : std::array<size_t, 2>{4, std::numeric_limits<size_t>::max()}) {
        EXPECT_FALSE(state.ExtractSubtree(kGenericTag, model, attributes, 4, root).ok());
        EXPECT_FALSE(state.IsValid());
        gfx::renderer::RenderParameters parameters{};
        EXPECT_EQ(gfx::renderer::Render({}, gfx::renderer::VertexKind::kVanilla,
                                       state, parameters).code(),
                  absl::StatusCode::kFailedPrecondition);
    }
    ASSERT_TRUE(state.ExtractSubtree(kGenericTag, model, attributes, 4, 1).ok());
    state.Invalidate();
    EXPECT_FALSE(state.IsValid());
    attributes[1].cubes_hidden = 1;
    auto empty = state.ExtractSubtree(kGenericTag, model, attributes, 4, 1);
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(state.IsValid());
    EXPECT_EQ(empty->vertex_count, 0);
    EXPECT_EQ(empty->locator_count, 1);
    attributes[1].children_hidden = 1;
    empty = state.ExtractSubtree(kGenericTag, model, attributes, 4, 1);
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty->locator_count, 0);
    attributes[1].scale[0] = 0;
    empty = state.ExtractSubtree(kGenericTag, model, attributes, 4, 1);
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty->vertex_count, 0);
    EXPECT_EQ(empty->locator_count, 0);
    EXPECT_FALSE(state.ExtractSubtree(kGenericTag, model, attributes, 3, 1).ok());
    EXPECT_FALSE(state.IsValid());
}

TEST(ModelStateTest, ExtractsIndependentBoneRenderAttributes) {
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    attributes[0].color = static_cast<float>(0x030201);
    attributes[0].transparency_glow = static_cast<float>(0x047F);
    attributes[1].color = static_cast<float>(0x060504);
    attributes[1].transparency_glow = static_cast<float>(0xFFFF);
    gfx::renderer::ModelState state;

    auto extracted =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(extracted.ok()) << extracted.status();

    const auto poses = state.PoseView().bone_poses;
    EXPECT_EQ(poses[0].color.components.r, 1);
    EXPECT_EQ(poses[0].color.components.g, 2);
    EXPECT_EQ(poses[0].color.components.b, 3);
    EXPECT_EQ(poses[0].color.components.a, 127);
    EXPECT_EQ(poses[0].glowing, 4);

    EXPECT_EQ(poses[1].color.components.r, 4);
    EXPECT_EQ(poses[1].color.components.g, 5);
    EXPECT_EQ(poses[1].color.components.b, 6);
    EXPECT_EQ(poses[1].color.components.a, 255);
    EXPECT_EQ(poses[1].glowing, 0xFF);
}

TEST(ModelStateTest, RejectsInvalidPackedBoneRenderAttributes) {
    auto model = MakeModel();

    {
        auto attributes = MakeAttributes();
        attributes[0].color = 0.5f;
        gfx::renderer::ModelState state;
        auto extracted =
            state.Extract(kGenericTag, model, attributes, attributes.size());
        EXPECT_FALSE(extracted.ok());
    }

    {
        auto attributes = MakeAttributes();
        attributes[0].color = 16777216.0f;
        gfx::renderer::ModelState state;
        auto extracted =
            state.Extract(kGenericTag, model, attributes, attributes.size());
        EXPECT_FALSE(extracted.ok());
    }

    {
        auto attributes = MakeAttributes();
        attributes[0].transparency_glow = static_cast<float>(0x10FF);
        gfx::renderer::ModelState state;
        auto extracted =
            state.Extract(kGenericTag, model, attributes, attributes.size());
        EXPECT_FALSE(extracted.ok());
    }
}

TEST(ModelStateTest, DetectsEqualSizedBoneIndexChange) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    gfx::renderer::ModelState state;

    attributes[1].cubes_hidden = 1.0f;
    auto first =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(first.ok()) << first.status();
    EXPECT_TRUE(first->schedule_updated);
    EXPECT_TRUE(std::ranges::equal(state.PoseView().render_bone_indices,
                                   std::array<uint16_t, 1>{3}));

    attributes[1].cubes_hidden = 0.0f;
    attributes[3].cubes_hidden = 1.0f;
    auto changed =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(changed.ok()) << changed.status();
    EXPECT_TRUE(changed->schedule_updated);
    EXPECT_TRUE(std::ranges::equal(state.PoseView().render_bone_indices,
                                   std::array<uint16_t, 1>{1}));

    auto unchanged =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(unchanged.ok()) << unchanged.status();
    EXPECT_FALSE(unchanged->schedule_updated);
}

TEST(ModelStateTest, PrunesHiddenInvalidAndZeroScaleSubtrees) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    gfx::renderer::ModelState state;

    attributes[1].children_hidden = 1.0f;
    auto children_hidden =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(children_hidden.ok()) << children_hidden.status();
    EXPECT_EQ(children_hidden->vertex_count, 8);
    EXPECT_EQ(children_hidden->locator_count, 0);

    attributes[1].children_hidden = 0.0f;
    attributes[1].scale[0] = 0.0f;
    auto zero_scale =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(zero_scale.ok()) << zero_scale.status();
    EXPECT_EQ(zero_scale->vertex_count, 4);
    EXPECT_EQ(zero_scale->locator_count, 0);
    EXPECT_TRUE(std::ranges::equal(state.PoseView().render_bone_indices,
                                   std::array<uint16_t, 1>{3}));

    attributes = MakeAttributes();
    attributes[0].rotation[0] = std::numeric_limits<float>::infinity();
    auto non_finite =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(non_finite.ok()) << non_finite.status();
    EXPECT_EQ(non_finite->vertex_count, 0);
    EXPECT_EQ(non_finite->locator_count, 0);

    attributes = MakeAttributes();
    attributes[2].locator_sequence = 1.5f;
    auto invalid_locator =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    EXPECT_EQ(invalid_locator.status().code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_FALSE(state.IsValid());
}

TEST(ModelStateTest, MaintainsBoneNormalInParallel) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    gfx::renderer::ModelState state;

    attributes[0].scale[0] = -2.0f;
    attributes[0].scale[1] = -2.0f;
    attributes[0].scale[2] = -2.0f;
    auto negative_uniform =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(negative_uniform.ok()) << negative_uniform.status();
    const auto& negative_pose = state.PoseView().bone_poses[0];
    EXPECT_FLOAT_EQ(negative_pose.normal[0][0], -1.0f);
    EXPECT_FLOAT_EQ(negative_pose.normal[1][1], -1.0f);
    EXPECT_FLOAT_EQ(negative_pose.normal[2][2], -1.0f);
    EXPECT_TRUE(negative_pose.uniform_scale);
    EXPECT_FLOAT_EQ(negative_pose.tangent_orientation, -1.0f);
    EXPECT_FLOAT_EQ(negative_pose.normal_scale, 2.0f);

    attributes[0].scale[0] = -2.0f;
    attributes[0].scale[1] = 2.0f;
    attributes[0].scale[2] = 2.0f;
    auto signed_uniform =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(signed_uniform.ok()) << signed_uniform.status();
    const auto& signed_pose = state.PoseView().bone_poses[0];
    EXPECT_FLOAT_EQ(signed_pose.normal[0][0], -1.0f);
    EXPECT_FLOAT_EQ(signed_pose.normal[1][1], 1.0f);
    EXPECT_FLOAT_EQ(signed_pose.normal[2][2], 1.0f);
    EXPECT_TRUE(signed_pose.uniform_scale);
    EXPECT_FLOAT_EQ(signed_pose.tangent_orientation, -1.0f);
    EXPECT_FLOAT_EQ(signed_pose.normal_scale, 2.0f);

    attributes[0].scale[0] = -2.0f;
    attributes[0].scale[1] = 3.0f;
    attributes[0].scale[2] = 4.0f;
    auto non_uniform =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(non_uniform.ok()) << non_uniform.status();
    const auto& non_uniform_pose = state.PoseView().bone_poses[0];
    EXPECT_FLOAT_EQ(non_uniform_pose.normal[0][0], -0.5f);
    EXPECT_FLOAT_EQ(non_uniform_pose.normal[1][1], 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(non_uniform_pose.normal[2][2], 0.25f);
    EXPECT_FALSE(non_uniform_pose.uniform_scale);
    EXPECT_FLOAT_EQ(non_uniform_pose.tangent_orientation, -1.0f);
    EXPECT_FLOAT_EQ(non_uniform_pose.normal_scale, 1.0f);
}

TEST(ModelStateTest, SnapshotRendersRepeatedlyWithTrustedNormalizedNormal) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    gfx::renderer::ModelState state;
    auto extracted =
        state.Extract(kGenericTag, model, attributes, attributes.size());
    ASSERT_TRUE(extracted.ok()) << extracted.status();

    gfx::renderer::RenderParameters parameters{};
    glm_mat4_identity(parameters.model);
    glm_mat4_identity(parameters.view);
    glm_mat4_identity(parameters.projection);
    glm_mat3_identity(parameters.normal);
    parameters.ctx = gfx::renderer::RenderContext::kLevel;
    parameters.light = 0x1234abcd;
    parameters.overlay = 0x01020304;
    parameters.color.packed = 0xffffffff;
    std::vector<Byte> first(extracted->vertex_count *
                            gfx::renderer::vertex::VanillaVertex::kSize);
    std::vector<Byte> second(first.size());

    ASSERT_TRUE(gfx::renderer::Render(first, gfx::renderer::VertexKind::kVanilla, state,
                                 parameters)
                    .ok());
    ASSERT_TRUE(gfx::renderer::Render(second, gfx::renderer::VertexKind::kVanilla, state,
                                 parameters)
                    .ok());
    EXPECT_EQ(first, second);
    const auto* vertices =
        reinterpret_cast<const gfx::renderer::vertex::VanillaVertex*>(first.data());
    EXPECT_EQ(vertices[0].light, parameters.light);
    EXPECT_EQ(vertices[0].normal & 0xffU, 0U);
    EXPECT_EQ((vertices[0].normal >> 16) & 0xffU, 0U);
    const auto packed_y = (vertices[0].normal >> 8) & 0xffU;
    EXPECT_TRUE(packed_y == 0x7fU || packed_y == 0x81U);

    parameters.ctx = gfx::renderer::RenderContext::kIrisShadow;
    std::vector<Byte> shadow(extracted->vertex_count *
                             gfx::renderer::vertex::Iris56Vertex::kSize);
    ASSERT_TRUE(gfx::renderer::Render(shadow, gfx::renderer::VertexKind::kIris56, state,
                                 parameters)
                    .ok());
    const auto* shadow_vertices =
        reinterpret_cast<const gfx::renderer::vertex::Iris56Vertex*>(shadow.data());
    EXPECT_NE(shadow_vertices[0].normal, 0U);
    EXPECT_EQ(shadow_vertices[0].color, 0U);
    EXPECT_EQ(shadow_vertices[0].tangent, 0U);
}

TEST(ModelStateTest, RenderReportsFailureProvenanceAndRetainsScheduleAcrossFrames) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeModel();
    auto attributes = MakeAttributes();
    gfx::renderer::ModelState state;
    auto extracted = state.ExtractSubtree(kGenericTag, model, attributes, attributes.size(), 1);
    ASSERT_TRUE(extracted.ok());
    EXPECT_TRUE(extracted->schedule_updated);
    gfx::renderer::RenderParameters parameters{};
    glm_mat4_identity(parameters.model);
    glm_mat4_identity(parameters.view);
    glm_mat4_identity(parameters.projection);
    glm_mat3_identity(parameters.normal);
    parameters.ctx = gfx::renderer::RenderContext::kLevel;
    parameters.color.packed = 0xffffffff;
    std::vector<Byte> output(extracted->vertex_count * gfx::renderer::vertex::VanillaVertex::kSize);
    gfx::renderer::RenderResult result;
    EXPECT_FALSE(gfx::renderer::Render({}, gfx::renderer::VertexKind::kVanilla, state, parameters, result).ok());
    EXPECT_EQ(result, gfx::renderer::RenderResult::kOutputRejected);
    EXPECT_TRUE(state.IsValid());
    parameters.normal[0][0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla, state, parameters, result).ok());
    EXPECT_EQ(result, gfx::renderer::RenderResult::kOutputRejected);
    glm_mat3_identity(parameters.normal);
    ASSERT_TRUE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla, state, parameters, result).ok());
    EXPECT_EQ(result, gfx::renderer::RenderResult::kSuccess);
    EXPECT_TRUE(state.IsValid());
    attributes[1].position[0] += 16;
    auto next_frame = state.ExtractSubtree(kGenericTag, model, attributes, attributes.size(), 1);
    ASSERT_TRUE(next_frame.ok());
    EXPECT_FALSE(next_frame->schedule_updated);
    auto& damaged = const_cast<gfx::renderer::BonePose&>(state.PoseView().bone_poses[1]);
    damaged.pose[0][0] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla, state, parameters, result).ok());
    EXPECT_EQ(result, gfx::renderer::RenderResult::kStateInvalid);
    ASSERT_TRUE(state.ExtractSubtree(kGenericTag, model, attributes, attributes.size(), 1).ok());
    ASSERT_TRUE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla, state, parameters, result).ok());
    state.Invalidate();
    EXPECT_FALSE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla, state, parameters, result).ok());
    EXPECT_EQ(result, gfx::renderer::RenderResult::kStateInvalid);
}

TEST(ModelStateTest, ProductionPolicyRendersPrewakeAndWorkerReadySchedules) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    gfx::renderer::RenderParameters parameters{};
    glm_mat4_identity(parameters.model);
    glm_mat4_identity(parameters.view);
    glm_mat4_identity(parameters.projection);
    glm_mat3_identity(parameters.normal);
    parameters.ctx = gfx::renderer::RenderContext::kLevel;
    parameters.light = 0x1234abcd;
    parameters.color.packed = 0xffffffff;

    const auto run_case =
        [&](size_t bone_count, gfx::renderer::RenderSchedulingMode expected_mode,
            size_t repetitions, gfx::renderer::ModelState& state,
            std::vector<Byte>& output) {
        auto model = MakeFlatModel(bone_count);
        auto attributes = MakeFlatAttributes(bone_count);
        auto extracted =
            state.Extract(kGenericTag, model, attributes, attributes.size());
        if (!extracted.ok()) {
            ADD_FAILURE() << extracted.status();
            return false;
        }
        if (state.Schedule().mode != expected_mode ||
            extracted->vertex_count != bone_count * 4) {
            ADD_FAILURE() << "Unexpected production schedule.";
            return false;
        }

        output.resize(extracted->vertex_count *
                      gfx::renderer::vertex::VanillaVertex::kSize);
        for (size_t repetition = 0; repetition < repetitions; ++repetition) {
            const auto status =
                gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla,
                                 state, parameters);
            if (!status.ok()) {
                ADD_FAILURE() << status;
                return false;
            }
        }

        const auto* vertices =
            reinterpret_cast<const gfx::renderer::vertex::VanillaVertex*>(
                output.data());
        for (size_t bone_index = 0; bone_index < bone_count; ++bone_index) {
            const auto& first_vertex = vertices[bone_index * 4];
            EXPECT_FLOAT_EQ(first_vertex.pos[0],
                            static_cast<float>(bone_index));
            EXPECT_FLOAT_EQ(first_vertex.pos[1], 0.0f);
            EXPECT_FLOAT_EQ(first_vertex.pos[2], 0.0f);
            EXPECT_EQ(first_vertex.light, parameters.light);
        }
        return true;
    };

    gfx::renderer::ModelState prewake_state;
    std::vector<Byte> prewake_output;
    ASSERT_TRUE(run_case(9, gfx::renderer::RenderSchedulingMode::kSerialPrewake, 8,
                         prewake_state, prewake_output));

    gfx::renderer::ModelState worker_ready_state;
    std::vector<Byte> worker_ready_output;
    ASSERT_TRUE(run_case(224, gfx::renderer::RenderSchedulingMode::kWorkerReadySpin,
                         32, worker_ready_state, worker_ready_output));

    auto& last_pose = const_cast<gfx::renderer::BonePose&>(
        worker_ready_state.PoseView().bone_poses.back());
    const auto previous = last_pose.pose[0][0];
    last_pose.pose[0][0] = std::numeric_limits<float>::infinity();
    const auto error =
        gfx::renderer::Render(worker_ready_output, gfx::renderer::VertexKind::kVanilla,
                         worker_ready_state, parameters);
    EXPECT_EQ(error.code(), absl::StatusCode::kInternal);
    last_pose.pose[0][0] = previous;
    EXPECT_TRUE(gfx::renderer::Render(worker_ready_output,
                                 gfx::renderer::VertexKind::kVanilla,
                                 worker_ready_state, parameters)
                    .ok());
}

TEST(ModelStateTest, CompositionOverflowRetainsPublicationAndDamageWinsInEveryMode) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    using Mode = gfx::renderer::RenderSchedulingMode;
    using Result = gfx::renderer::RenderResult;
    for (const auto mode : {Mode::kInline, Mode::kSerialLateWake,
                            Mode::kSerialPrewake, Mode::kWorkerReadySpin}) {
        SCOPED_TRACE(static_cast<int>(mode));
        const auto bone_count = mode == Mode::kInline ? 2 : 224;
        auto model = MakeFlatModel(bone_count);
        auto attributes = MakeFlatAttributes(bone_count);
        attributes[0].scale[0] = 4;
        gfx::renderer::ModelState state;
        auto extracted = state.Extract(kGenericTag, model, attributes, attributes.size());
        ASSERT_TRUE(extracted.ok());
        auto& schedule = const_cast<gfx::renderer::RenderSchedule&>(state.Schedule());
        // Keep the real task/owner partition, varying only the execution policy.
        schedule.mode = mode;
        std::vector<Byte> output(extracted->vertex_count *
                                gfx::renderer::vertex::VanillaVertex::kSize);
        gfx::renderer::RenderParameters parameters{};
        glm_mat4_identity(parameters.model);
        glm_mat4_identity(parameters.view);
        glm_mat4_identity(parameters.projection);
        glm_mat3_identity(parameters.normal);
        parameters.ctx = gfx::renderer::RenderContext::kLevel;
        parameters.color.packed = 0xffffffff;
        Result result;
        ASSERT_TRUE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla,
                                         state, parameters, result).ok());
        const auto* published = state.PoseView().bone_poses.data();
        parameters.model[0][0] = std::numeric_limits<float>::max() / 2;
        parameters.normal[0][0] = 1 / parameters.model[0][0];
        ASSERT_FALSE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla,
                                          state, parameters, result).ok());
        EXPECT_EQ(result, Result::kOutputRejected);
        EXPECT_TRUE(state.IsValid());
        EXPECT_EQ(published, state.PoseView().bone_poses.data());
        auto& damaged = const_cast<gfx::renderer::BonePose&>(
            state.PoseView().bone_poses.back());
        const auto previous = damaged.pose[0][0];
        damaged.pose[0][0] = std::numeric_limits<float>::infinity();
        ASSERT_FALSE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla,
                                          state, parameters, result).ok());
        EXPECT_EQ(result, Result::kStateInvalid);
        damaged.pose[0][0] = previous;
        glm_mat4_identity(parameters.model);
        glm_mat3_identity(parameters.normal);
        ASSERT_TRUE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla,
                                         state, parameters, result).ok());
        EXPECT_EQ(result, Result::kSuccess);
        EXPECT_EQ(published, state.PoseView().bone_poses.data());
    }
}

TEST(ModelStateTest, ThreePersistentOutputsReuseSchedulesAndBoundPoseStorage) {
    InitCpuInfo();
#ifdef YSM_X64
    // MakeFlatModel builds B128 groups regardless of the host ISA.
    simd::Mock(simd::Type::SSE41);
#endif
    auto model = MakeFlatModel(96);
    auto attributes = MakeFlatAttributes(96);
    std::array<gfx::renderer::ModelState, 3> states;
    std::array<std::vector<Byte>, 3> buffers;
    std::array<const gfx::renderer::BonePose*, 3> poses{};
    size_t extracts = 0;
    size_t rebuilds = 0;
    for (size_t index = 0; index < states.size(); ++index) {
        auto result = states[index].ExtractSubtree(
                kGenericTag, model, attributes, attributes.size(), index);
        ASSERT_TRUE(result.ok()) << result.status();
        ++extracts;
        rebuilds += result->schedule_updated;
        ASSERT_EQ(result->vertex_count, 4);
        EXPECT_EQ(states[index].PoseView().bone_poses.size(), 96);
        poses[index] = states[index].PoseView().bone_poses.data();
        buffers[index].resize(4 * gfx::renderer::vertex::VanillaVertex::kSize);
    }
    EXPECT_EQ(extracts, 3);
    EXPECT_EQ(rebuilds, 3);
    EXPECT_NE(poses[0], poses[1]);
    EXPECT_NE(poses[1], poses[2]);

    gfx::renderer::RenderParameters parameters{};
    glm_mat4_identity(parameters.model);
    glm_mat4_identity(parameters.view);
    glm_mat4_identity(parameters.projection);
    glm_mat3_identity(parameters.normal);
    parameters.ctx = gfx::renderer::RenderContext::kLevel;
    parameters.color.packed = 0xffffffff;
    for (const auto index : std::array<size_t, 5>{0, 1, 2, 0, 1}) {
        ASSERT_TRUE(gfx::renderer::Render(buffers[index],
                gfx::renderer::VertexKind::kVanilla, states[index], parameters).ok());
        EXPECT_TRUE(states[index].IsValid());
        EXPECT_EQ(states[index].PoseView().bone_poses.data(), poses[index]);
    }
    EXPECT_EQ(extracts, 3);
    EXPECT_EQ(rebuilds, 3);

    attributes[0].position[0] = 0.25f;
    for (size_t index = 0; index < states.size(); ++index) {
        auto result = states[index].ExtractSubtree(
                kGenericTag, model, attributes, attributes.size(), index);
        ASSERT_TRUE(result.ok()) << result.status();
        ++extracts;
        rebuilds += result->schedule_updated;
        EXPECT_FALSE(result->schedule_updated);
        EXPECT_EQ(states[index].PoseView().bone_poses.data(), poses[index]);
    }
    EXPECT_EQ(extracts, 6);
    EXPECT_EQ(rebuilds, 3);

    attributes[0].cubes_hidden = 1.0f;
    for (size_t index = 0; index < states.size(); ++index) {
        auto result = states[index].ExtractSubtree(
                kGenericTag, model, attributes, attributes.size(), index);
        ASSERT_TRUE(result.ok()) << result.status();
        ++extracts;
        rebuilds += result->schedule_updated;
        EXPECT_EQ(result->schedule_updated, index == 0);
        EXPECT_EQ(result->vertex_count, index == 0 ? 0 : 4);
        EXPECT_EQ(states[index].PoseView().bone_poses.data(), poses[index]);
    }
    EXPECT_EQ(extracts, 9);
    EXPECT_EQ(rebuilds, 4);
}
}  // namespace ysm::test
