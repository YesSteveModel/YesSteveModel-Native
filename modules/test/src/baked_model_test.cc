#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <ylt/struct_pb.hpp>

#include "cpu.h"
#include "gfx/bake/baked_model.h"
#include "gfx/bake/baked_serializer.h"
#include "gfx/renderer/model_state.h"
#include "gfx/renderer/render.h"
#include "renderer/vertex/vanilla.h"
#include "proto/asset/model/data/geo_model.proto.h"
#include "scope_guard.h"

namespace ysm::test {
namespace {

proto::asset::model::data::CubeLegacy MakeQuad(float max_uv) {
    proto::asset::model::data::CubeLegacy cube;
    cube.faceCount = 1;
    cube.pos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    cube.pos_indices = {0, 1, 2, 3};
    cube.uv = {0, 0, max_uv, 0, max_uv, max_uv, 0, max_uv};
    cube.uv_indices = {0, 1, 2, 3};
    cube.normal = {0, 0, 1};
    return cube;
}

proto::asset::model::data::CubeLegacy MakeSixFaceCube(float max_uv) {
    auto cube = MakeQuad(max_uv);
    cube.faceCount = 6;
    const auto pos_indices = cube.pos_indices;
    const auto uv_indices = cube.uv_indices;
    const auto normal = cube.normal;
    for (size_t i = 1; i < 6; ++i) {
        cube.pos_indices.insert(cube.pos_indices.end(), pos_indices.begin(),
                                pos_indices.end());
        cube.uv_indices.insert(cube.uv_indices.end(), uv_indices.begin(),
                               uv_indices.end());
        cube.normal.insert(cube.normal.end(), normal.begin(), normal.end());
    }
    return cube;
}

std::string SerializeSingleCube(proto::asset::model::data::CubeLegacy cube) {
    proto::asset::model::data::GeoModel model;
    model.bones.resize(1);
    model.bones[0].name = "root";
    model.bones[0].pivot = {0, 0, 0};
    model.bones[0].rotate = {0, 0, 0};
    model.bones[0].cubeCount = 1;
    model.cubes.cubes_legacy.push_back(std::move(cube));
    std::string protobuf;
    struct_pb::to_pb(model, protobuf);
    return protobuf;
}

TEST(BakedModelTest, BakeRoundTrip) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif

    proto::asset::model::data::GeoModel model;
    model.bones.resize(2);
    model.bones[0].name = "child";
    model.bones[0].parent = "root";
    model.bones[0].pivot = {16, 0, 0};
    model.bones[0].rotate = {0, 0, 0};
    model.bones[1].name = "root";
    model.bones[1].pivot = {0, 16, 0};
    model.bones[1].rotate = {0, 0, 0};
    model.bones[0].cubeCount = 2;
    model.bones[1].cubeCount = 1;
    model.cubes.cubes_legacy = {MakeQuad(0.5f), MakeQuad(0.5f), MakeQuad(1.0f)};
    std::string protobuf;
    struct_pb::to_pb(model, protobuf);

    std::array<gfx::bake::Pixel, 4> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 0},
    };
    gfx::bake::Texture texture(pixels.data(), 2, 2);
    auto baked_model = gfx::bake::BakeModel(StrBuf(protobuf), texture,
                                       {.origin_ver = 29,
                                        .force_culling = false,
                                        .force_translucent = false,
                                        .has_pbr = true});
    ASSERT_TRUE(baked_model.ok()) << baked_model.status();
    auto baked = gfx::bake::SerializeBakedModel(**baked_model);

    auto restored = gfx::bake::ReadBakedModel(baked);
    ASSERT_TRUE(restored.ok()) << restored.status();
    auto reserialized = gfx::bake::SerializeBakedModel(**restored);
    ASSERT_EQ(baked.size(), reserialized.size());
    EXPECT_EQ(std::memcmp(baked.data(), reserialized.data(), baked.size()), 0);
    const auto& info = (*baked_model)->Info();
    const auto& bones = (*baked_model)->Bones();
    ASSERT_EQ(bones.list.size(), 2);
    EXPECT_EQ(bones.sorted_bone_indices, (std::vector<uint16_t>{1, 0}));
    EXPECT_FALSE(bones.list[0].solid);
    EXPECT_TRUE(bones.list[1].solid);
    EXPECT_EQ(bones.list[0].parent_index, UINT32_MAX);
    EXPECT_EQ(bones.list[0].subtree_end, 2);
    EXPECT_EQ(bones.list[0].depth, 0);
    EXPECT_EQ(bones.list[0].pivot, (std::array<float, 3>{0, 16, 0}));
    EXPECT_EQ(bones.list[1].parent_index, 0);
    EXPECT_EQ(bones.list[1].subtree_end, 2);
    EXPECT_EQ(bones.list[1].depth, 1);
    EXPECT_EQ(bones.list[1].pivot, (std::array<float, 3>{16, 0, 0}));
    EXPECT_FALSE(info.gui_no_shadow);
    EXPECT_TRUE(info.has_pbr);

    const auto& cubes = (*baked_model)->Cubes<simd::Width::B128>();
    ASSERT_EQ(cubes.cutout_no_culling.size(), 1);
    EXPECT_EQ(cubes.cutout_no_culling[0].cube_count, 2);
    EXPECT_EQ(cubes.cutout_no_culling[0].bone_index, 1);
    const auto& cutout = cubes.cutout_no_culling[0];
    EXPECT_NEAR(cutout.tangent[0][0], 1.0f, 0.0001f);
    EXPECT_NEAR(cutout.tangent[1][0], 0.0f, 0.0001f);
    EXPECT_NEAR(cutout.tangent[2][0], 0.0f, 0.0001f);
    EXPECT_FLOAT_EQ(cutout.tangent[3][0], -1.0f);
    EXPECT_FLOAT_EQ(cutout.plane_d[0], 0.0f);
    EXPECT_FLOAT_EQ(cutout.winding_sign[0], 1.0f);
    ASSERT_EQ(cubes.translucent.size(), 1);
    EXPECT_EQ(cubes.translucent[0].bone_index, 0);
    const auto& translucent = cubes.translucent[0];
    EXPECT_NEAR(translucent.center[0][0], 0.5f, 0.0001f);
    EXPECT_NEAR(translucent.center[1][0], 0.5f, 0.0001f);
    EXPECT_NEAR(translucent.cube_attr[0].quad_attr[0].mid_uv[0], 0.5f, 0.0001f);
    EXPECT_NEAR(translucent.cube_attr[0].quad_attr[0].mid_uv[1], 0.5f, 0.0001f);
    EXPECT_EQ(bones.list[1].cutout_no_culling.cube_indices.size(), 1);
    EXPECT_EQ(bones.list[0].translucent.cube_indices.size(), 1);
    const auto& cutout_bone = bones.list[1].cutout_no_culling;
    EXPECT_EQ(cutout_bone.cube_count, 2);
    ASSERT_EQ(cutout_bone.cube_group_info.size(), 1);
    EXPECT_EQ(cutout_bone.cube_group_info[0].cube_count, 2);
    EXPECT_EQ(cutout_bone.cube_group_info[0].quad_count, 2);
    EXPECT_EQ(cutout_bone.cube_group_info[0].quad_count_after_culling, 2);

    BufferManaged corrupted(baked);
    corrupted.data()[0] ^= 0xff;
    EXPECT_FALSE(gfx::bake::ReadBakedModel(corrupted).ok());
    EXPECT_FALSE(gfx::bake::ReadBakedModel(
                     BufferViewR(baked.data(), baked.size() - 1))
                     .ok());
}

TEST(BakedModelTest, LegacyFlags) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    std::array<gfx::bake::Pixel, 4> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 0},
    };
    gfx::bake::Texture texture(pixels.data(), 2, 2);

    auto opaque_data = SerializeSingleCube(MakeQuad(0.5f));
    auto alex = gfx::bake::BakeModel(
        StrBuf(opaque_data), texture,
        {.origin_ver = 29, .force_culling = false, .force_translucent = true});
    ASSERT_TRUE(alex.ok()) << alex.status();
    auto alex_baked = gfx::bake::SerializeBakedModel(**alex);
    auto alex_model = gfx::bake::ReadBakedModel(alex_baked);
    ASSERT_TRUE(alex_model.ok()) << alex_model.status();
    const auto& alex_cubes = (*alex_model)->Cubes<simd::Width::B128>();
    EXPECT_TRUE(alex_cubes.cutout_no_culling.empty());
    EXPECT_EQ(alex_cubes.translucent.size(), 1);

    auto full_cube_data = SerializeSingleCube(MakeSixFaceCube(1.0f));
    auto forced = gfx::bake::BakeModel(
        StrBuf(full_cube_data), texture,
        {.origin_ver = 29, .force_culling = true, .force_translucent = false});
    ASSERT_TRUE(forced.ok()) << forced.status();
    auto forced_baked = gfx::bake::SerializeBakedModel(**forced);
    auto forced_model = gfx::bake::ReadBakedModel(forced_baked);
    ASSERT_TRUE(forced_model.ok()) << forced_model.status();
    const auto& forced_cubes = (*forced_model)->Cubes<simd::Width::B128>();
    EXPECT_TRUE(forced_cubes.translucent.empty());
    ASSERT_EQ(forced_cubes.translucent_culling.size(), 1);
    EXPECT_EQ(forced_cubes.translucent_culling[0]
                  .cube_attr[0]
                  .quad_count_after_culling,
              3);
}

TEST(BakedModelTest, SingleQuadVertexLayout) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto cube = MakeQuad(0.5f);
    cube.pos_indices = {2, 0, 3, 1};
    cube.normal = {0, 0, -1};
    const auto model_data = SerializeSingleCube(std::move(cube));
    const std::array<gfx::bake::Pixel, 4> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 2, 2);

    auto baked = gfx::bake::BakeModel(StrBuf(model_data), texture,
                                 {.origin_ver = 29,
                                  .force_culling = false,
                                  .force_translucent = false,
                                  .has_pbr = true});
    ASSERT_TRUE(baked.ok()) << baked.status();
    auto restored = gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(**baked));
    ASSERT_TRUE(restored.ok()) << restored.status();

    const auto& groups =
        (*restored)->Cubes<simd::Width::B128>().cutout_no_culling;
    ASSERT_EQ(groups.size(), 1);
    const auto& group = groups[0];
    EXPECT_EQ(group.cube_count, 1);
    EXPECT_EQ(group.cube_attr[0].quad_count, 1);
    EXPECT_FLOAT_EQ(group.normal[0][0], 0.0f);
    EXPECT_FLOAT_EQ(group.normal[1][0], 0.0f);
    EXPECT_FLOAT_EQ(group.normal[2][0], -1.0f);
    EXPECT_FLOAT_EQ(group.tangent[3][0], -1.0f);
    for (uint32_t vertex = 0; vertex < 4; ++vertex) {
        EXPECT_EQ(group.vertex_index[vertex][0], vertex);
    }
    EXPECT_FLOAT_EQ(group.pos[0][0], 1.0f);
    EXPECT_FLOAT_EQ(group.pos[1][0], 1.0f);
    EXPECT_FLOAT_EQ(group.pos[0][1], 0.0f);
    EXPECT_FLOAT_EQ(group.pos[1][1], 0.0f);
    EXPECT_FLOAT_EQ(group.pos[0][2], 0.0f);
    EXPECT_FLOAT_EQ(group.pos[1][2], 1.0f);
    EXPECT_FLOAT_EQ(group.pos[0][3], 1.0f);
    EXPECT_FLOAT_EQ(group.pos[1][3], 0.0f);
}

TEST(BakedModelTest, GeneratesStablePreorderForUnsortedForest) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    proto::asset::model::data::GeoModel model;
    model.bones.resize(6);
    const auto set_bone = [&](size_t index, std::string name,
                              std::string parent = {}) {
        model.bones[index].name = std::move(name);
        model.bones[index].parent = std::move(parent);
        model.bones[index].pivot = {static_cast<float>(index), 0, 0};
        model.bones[index].rotate = {0, 0, 0};
    };
    set_bone(0, "child_b", "root_a");
    set_bone(1, "root_b");
    set_bone(2, "grandchild", "child_a");
    set_bone(3, "child_a", "root_a");
    set_bone(4, "root_a");
    set_bone(5, "child_c", "root_b");

    std::string protobuf;
    struct_pb::to_pb(model, protobuf);
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);
    auto baked = gfx::bake::BakeModel(StrBuf(protobuf), texture, {});
    ASSERT_TRUE(baked.ok()) << baked.status();

    const auto& bones = (*baked)->Bones();
    EXPECT_EQ(bones.sorted_bone_indices,
              (std::vector<uint16_t>{1, 5, 4, 0, 3, 2}));
    ASSERT_EQ(bones.list.size(), 6);
    EXPECT_EQ(bones.list[0].parent_index, UINT32_MAX);
    EXPECT_EQ(bones.list[0].subtree_end, 2);
    EXPECT_EQ(bones.list[0].depth, 0);
    EXPECT_EQ(bones.list[1].parent_index, 0);
    EXPECT_EQ(bones.list[1].subtree_end, 2);
    EXPECT_EQ(bones.list[1].depth, 1);
    EXPECT_EQ(bones.list[2].parent_index, UINT32_MAX);
    EXPECT_EQ(bones.list[2].subtree_end, 6);
    EXPECT_EQ(bones.list[3].parent_index, 2);
    EXPECT_EQ(bones.list[3].subtree_end, 4);
    EXPECT_EQ(bones.list[4].parent_index, 2);
    EXPECT_EQ(bones.list[4].subtree_end, 6);
    EXPECT_EQ(bones.list[5].parent_index, 4);
    EXPECT_EQ(bones.list[5].subtree_end, 6);
    EXPECT_EQ(bones.list[5].depth, 2);

    auto restored = gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(**baked));
    ASSERT_TRUE(restored.ok()) << restored.status();
    EXPECT_EQ((*restored)->Bones().sorted_bone_indices,
              bones.sorted_bone_indices);
}

TEST(BakedModelTest, PreservesEmptyNamedChildGeometry) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    proto::asset::model::data::GeoModel model;
    model.from_pb(SerializeSingleCube(MakeQuad(1.0f)));
    model.bones[0].name.clear();
    model.bones[0].parent = "root";
    model.bones[0].pivot = {16, 0, 0};
    model.bones.resize(2);
    model.bones[1].name = "root";
    model.bones[1].pivot = {0, 16, 0};
    model.bones[1].rotate = {0, 0, 0};
    std::string protobuf;
    struct_pb::to_pb(model, protobuf);
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);
    ASSERT_TRUE(gfx::bake::TryBakeModel(StrBuf(protobuf), texture, {}).ok());
    auto baked = gfx::bake::BakeModel(StrBuf(protobuf), texture, {});
    ASSERT_TRUE(baked.ok()) << baked.status();
    auto restored = gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(**baked));
    ASSERT_TRUE(restored.ok()) << restored.status();

    const auto& bones = (*restored)->Bones();
    EXPECT_EQ(bones.sorted_bone_indices, (std::vector<uint16_t>{1, 0}));
    ASSERT_EQ(bones.list.size(), 2);
    EXPECT_EQ(bones.list[0].parent_index, UINT32_MAX);
    EXPECT_EQ(bones.list[0].subtree_end, 2);
    EXPECT_EQ(bones.list[1].parent_index, 0);
    EXPECT_EQ(bones.list[1].depth, 1);
    EXPECT_EQ(bones.list[1].pivot, (std::array<float, 3>{16, 0, 0}));
    EXPECT_TRUE(bones.list[1].HasGeometry());
    EXPECT_EQ(bones.list[1].cutout_no_culling.cube_count, 1);
    const auto& groups =
        (*restored)->Cubes<simd::Width::B128>().cutout_no_culling;
    ASSERT_EQ(groups.size(), 1);
    EXPECT_EQ(groups[0].bone_index, 1);
    EXPECT_EQ(groups[0].cube_count, 1);
}

TEST(BakedModelTest, EmptyParentRemainsRootWithEmptyNamedBone) {
    InitCpuInfo();
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);
    for (const bool explicit_empty_parent : {false, true}) {
        SCOPED_TRACE(explicit_empty_parent);
        proto::asset::model::data::GeoModel model;
        model.from_pb(SerializeSingleCube(MakeQuad(1.0f)));
        model.bones[0].name.clear();
        model.bones.resize(2);
        model.bones[1].name = "other";
        model.bones[1].pivot = {0, 0, 0};
        model.bones[1].rotate = {0, 0, 0};
        if (explicit_empty_parent) {
            model.bones[0].parent = "";
            model.bones[1].parent = "";
        }
        std::string protobuf;
        struct_pb::to_pb(model, protobuf);
        ASSERT_TRUE(gfx::bake::TryBakeModel(StrBuf(protobuf), texture, {}).ok());
        auto baked = gfx::bake::BakeModel(StrBuf(protobuf), texture, {});
        ASSERT_TRUE(baked.ok()) << baked.status();
        const auto& bones = (*baked)->Bones();
        EXPECT_EQ(bones.sorted_bone_indices, (std::vector<uint16_t>{0, 1}));
        ASSERT_EQ(bones.list.size(), 2);
        for (size_t i = 0; i < bones.list.size(); ++i) {
            EXPECT_EQ(bones.list[i].parent_index, UINT32_MAX);
            EXPECT_EQ(bones.list[i].depth, 0);
            EXPECT_EQ(bones.list[i].subtree_end, i + 1);
        }
        EXPECT_TRUE(bones.list[0].HasGeometry());
    }
}

TEST(BakedModelTest, RejectsDuplicateBoneNamesIncludingEmpty) {
    InitCpuInfo();
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);
    for (const auto& name : std::array<std::string, 2>{"root", ""}) {
        SCOPED_TRACE(name);
        proto::asset::model::data::GeoModel model;
        model.bones.resize(2);
        for (auto& bone : model.bones) {
            bone.name = name;
            bone.parent = "";
            bone.pivot = {0, 0, 0};
            bone.rotate = {0, 0, 0};
        }
        std::string protobuf;
        struct_pb::to_pb(model, protobuf);
        EXPECT_EQ(gfx::bake::BakeModel(StrBuf(protobuf), texture, {}).status().code(),
                  absl::StatusCode::kInvalidArgument);
        EXPECT_EQ(gfx::bake::TryBakeModel(StrBuf(protobuf), texture, {}).code(),
                  absl::StatusCode::kInvalidArgument);
    }
}

TEST(BakedModelTest, RejectsInvalidHierarchyMetadata) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);
    const auto make_model = [] {
        proto::asset::model::data::GeoModel model;
        model.bones.resize(2);
        model.bones[0].name = "root";
        model.bones[0].pivot = {0, 0, 0};
        model.bones[0].rotate = {0, 0, 0};
        model.bones[1].name = "child";
        model.bones[1].parent = "root";
        model.bones[1].pivot = {0, 0, 0};
        model.bones[1].rotate = {0, 0, 0};
        return model;
    };
    const auto expect_invalid =
        [&](const proto::asset::model::data::GeoModel& model) {
        std::string protobuf;
        struct_pb::to_pb(model, protobuf);
        EXPECT_EQ(
            gfx::bake::BakeModel(StrBuf(protobuf), texture, {}).status().code(),
            absl::StatusCode::kInvalidArgument);
    };

    auto model = make_model();
    model.bones[1].pivot[0] = std::numeric_limits<float>::quiet_NaN();
    expect_invalid(model);

    model = make_model();
    model.bones[1].parent = "missing";
    expect_invalid(model);

    model = make_model();
    model.bones[0].parent = "root";
    expect_invalid(model);

    model = make_model();
    model.bones[0].parent = "child";
    expect_invalid(model);

    proto::asset::model::data::GeoModel too_many;
    too_many.bones.resize(static_cast<size_t>(UINT16_MAX) + 2ULL);
    expect_invalid(too_many);

    gfx::bake::BakedModelBones corrupt_bones;
    corrupt_bones.sorted_bone_indices = {0};
    corrupt_bones.list.resize(1);
    corrupt_bones.list[0].subtree_end = 0;
    gfx::bake::BakedModelCubes<simd::Width::B128> empty_cubes;
    gfx::bake::BakedModel corrupt({}, std::move(corrupt_bones),
                             std::move(empty_cubes));
    EXPECT_FALSE(gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(corrupt)).ok());

    gfx::bake::BakedModelBones invalid_depth_bones;
    invalid_depth_bones.sorted_bone_indices = {0, 1};
    invalid_depth_bones.list.resize(2);
    invalid_depth_bones.list[0].subtree_end = 2;
    invalid_depth_bones.list[1].parent_index = 0;
    invalid_depth_bones.list[1].subtree_end = 2;
    invalid_depth_bones.list[1].depth = 0;
    gfx::bake::BakedModelCubes<simd::Width::B128> invalid_depth_cubes;
    gfx::bake::BakedModel invalid_depth({}, std::move(invalid_depth_bones),
                                   std::move(invalid_depth_cubes));
    EXPECT_FALSE(
        gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(invalid_depth)).ok());
}

TEST(BakedModelTest, ReportsInvalidCubeLocationAndCondition) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto cube = MakeQuad(1.0f);
    cube.faceCount = 0;
    const auto model_data = SerializeSingleCube(std::move(cube));
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);

    const auto status = gfx::bake::BakeModel(StrBuf(model_data), texture, {}).status();

    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(status.message().find("bone[0] \"root\" cube[0]"),
              std::string_view::npos);
    EXPECT_NE(status.message().find("position index count does not equal "
                                    "face_count * 4 (face_count=0, got 4)"),
              std::string_view::npos);
}

TEST(BakedModelTest, RejectsCorruptSerializedBoneIndices) {
    const auto make_model = [](std::vector<uint16_t> indices) {
        gfx::bake::BakedModelBones bones;
        bones.sorted_bone_indices = std::move(indices);
        bones.list.resize(2);
        bones.list[0].subtree_end = 2;
        bones.list[1].parent_index = 0;
        bones.list[1].subtree_end = 2;
        bones.list[1].depth = 1;
        gfx::bake::BakedModelCubes<simd::Width::B128> cubes;
        return gfx::bake::BakedModel({}, std::move(bones), std::move(cubes));
    };

    auto wrong_size = make_model({0});
    EXPECT_FALSE(
        gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(wrong_size)).ok());
    auto duplicate = make_model({0, 0});
    EXPECT_FALSE(
        gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(duplicate)).ok());
    auto out_of_range = make_model({0, 2});
    EXPECT_FALSE(
        gfx::bake::ReadBakedModel(gfx::bake::SerializeBakedModel(out_of_range)).ok());
}

TEST(BakedModelTest, PreservesTrustedModelNormal) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    auto cube = MakeQuad(1.0f);
    cube.normal = {0, 2, 0};
    auto model_data = SerializeSingleCube(std::move(cube));
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);
    auto baked = gfx::bake::BakeModel(StrBuf(model_data), texture, {});
    ASSERT_TRUE(baked.ok()) << baked.status();
    const auto& groups = (*baked)->Cubes<simd::Width::B128>().cutout_no_culling;
    ASSERT_EQ(groups.size(), 1);
    EXPECT_FLOAT_EQ(groups[0].normal[0][0], 0.0f);
    EXPECT_FLOAT_EQ(groups[0].normal[1][0], 2.0f);
    EXPECT_FLOAT_EQ(groups[0].normal[2][0], 0.0f);
}

TEST(BakedModelTest, NonPbrCacheRestoresZeroTangents) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    const auto model_data = SerializeSingleCube(MakeQuad(1.0f));
    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    gfx::bake::Texture texture(pixels.data(), 1, 1);

    auto baked =
        gfx::bake::BakeModel(StrBuf(model_data), texture, {.has_pbr = false});
    ASSERT_TRUE(baked.ok()) << baked.status();
    const auto& direct = (*baked)->Cubes<simd::Width::B128>().cutout_no_culling[0];
    for (const auto& axis : direct.tangent) {
        for (const auto value : axis) {
            EXPECT_FLOAT_EQ(value, 0.0f);
        }
    }
    auto serialized = gfx::bake::SerializeBakedModel(**baked);

    auto restored = gfx::bake::ReadBakedModel(serialized);
    ASSERT_TRUE(restored.ok()) << restored.status();
    EXPECT_FALSE((*restored)->Info().has_pbr);
    const auto& group =
        (*restored)->Cubes<simd::Width::B128>().cutout_no_culling[0];
    EXPECT_FLOAT_EQ(group.tangent[0][0], 0.0f);
    EXPECT_FLOAT_EQ(group.tangent[3][0], 0.0f);
}

TEST(BakedModelTest, RejectsMismatchedSerializedVectorWidth) {
    InitCpuInfo();
#ifdef YSM_X64
    simd::Mock(simd::Type::SSE41);
#endif
    gfx::bake::BakedModel model(
        {}, {}, gfx::bake::BakedModelCubes<simd::Width::B256>{});
    EXPECT_FALSE(gfx::bake::ReadBakedModel(
                     gfx::bake::SerializeBakedModel(model))
                     .ok());
}

class DirectBakedModelTest : public testing::TestWithParam<simd::Type> {};

TEST_P(DirectBakedModelTest, KnownQuadPartitionsAndSurfaceData) {
    InitCpuInfo();
#ifdef YSM_X64
    if ((GetParam() == simd::Type::AVX2 && !kX86Features.avx2) ||
        (GetParam() == simd::Type::AVX512 &&
         !(kX86Features.avx512f && kX86Features.avx512vl && kX86Features.avx512bw))) {
        GTEST_SKIP() << "SIMD is unavailable on this host";
    }
#endif
    const auto previous = simd::kSupported;
    simd::Mock(GetParam());
    const auto restore = ScopeGuard([&] { simd::Mock(previous); });
    for (const bool pbr : {false, true}) {
        for (int partition = 0; partition < 4; ++partition) {
            const bool translucent = partition >= 2;
            const bool full_cube = partition == 0 || partition == 3;
            const auto bytes = SerializeSingleCube(
                full_cube ? MakeSixFaceCube(1.0f) : MakeQuad(1.0f));
            const std::array pixels{gfx::bake::Pixel{
                255, 255, 255, static_cast<uint8_t>(translucent ? 128 : 255)}};
            auto baked = gfx::bake::BakeModel(
                StrBuf(bytes), gfx::bake::Texture(pixels.data(), 1, 1),
                {.origin_ver = 29, .force_culling = partition == 3, .has_pbr = pbr});
            ASSERT_TRUE(baked.ok()) << baked.status();
            EXPECT_EQ((*baked)->Bones().sorted_bone_indices, (std::vector<uint16_t>{0}));
            const auto& bone = (*baked)->Bones().list[0];
            const std::array parts{&bone.cutout, &bone.cutout_no_culling,
                                   &bone.translucent, &bone.translucent_culling};
            for (size_t index = 0; index < parts.size(); ++index) {
                EXPECT_EQ(parts[index]->cube_count, index == partition ? 1 : 0);
            }
            EXPECT_EQ(parts[partition]->full_vertex_count, full_cube ? 24 : 4);
            (*baked)->VisitCubes([&](const auto& cubes) {
                const auto verify = [&](const auto& groups) {
                    ASSERT_EQ(groups.size(), 1);
                    const auto& group = groups[0];
                    EXPECT_FLOAT_EQ(group.pos[0][1], 1.0f);
                    EXPECT_FLOAT_EQ(group.normal[2][0], 1.0f);
                    EXPECT_FLOAT_EQ(group.cube_attr[0].quad_attr[0].uv[2][0], 1.0f);
                    EXPECT_FLOAT_EQ(group.tangent[0][0], pbr ? 1.0f : 0.0f);
                    EXPECT_FLOAT_EQ(group.tangent[3][0], pbr ? -1.0f : 0.0f);
                };
                switch (partition) {
                    case 0: verify(cubes.cutout); break;
                    case 1: verify(cubes.cutout_no_culling); break;
                    case 2: verify(cubes.translucent); break;
                    case 3: verify(cubes.translucent_culling); break;
                }
            });
            auto serialized = gfx::bake::SerializeBakedModel(**baked);
            auto restored = gfx::bake::ReadBakedModel(serialized);
            ASSERT_TRUE(restored.ok()) << restored.status();
            auto round_trip = gfx::bake::SerializeBakedModel(**restored);
            ASSERT_EQ(serialized.size(), round_trip.size());
            EXPECT_EQ(std::memcmp(serialized.data(), round_trip.data(), serialized.size()), 0);
            const std::shared_ptr<gfx::bake::BakedModel> direct(std::move(*baked));
            const std::shared_ptr<gfx::bake::BakedModel> reference(std::move(*restored));
            std::array<gfx::renderer::BoneAttribute, 1> attributes{};
            std::ranges::fill(attributes[0].scale, 1.0f);
            gfx::renderer::ModelState direct_state;
            gfx::renderer::ModelState reference_state;
            auto extracted = direct_state.Extract(
                simd::GenericTag{}, direct, attributes, attributes.size());
            auto reference_extract = reference_state.Extract(
                simd::GenericTag{}, reference, attributes, attributes.size());
            ASSERT_TRUE(extracted.ok()) << extracted.status();
            ASSERT_TRUE(reference_extract.ok()) << reference_extract.status();
            EXPECT_EQ(extracted->vertex_count, reference_extract->vertex_count);
            gfx::renderer::RenderParameters parameters{};
            glm_mat4_identity(parameters.model);
            glm_mat4_identity(parameters.view);
            glm_mat4_identity(parameters.projection);
            glm_mat3_identity(parameters.normal);
            parameters.ctx = gfx::renderer::RenderContext::kLevel;
            parameters.light = 0x1234abcd;
            parameters.color.packed = 0xffffffff;
            std::vector<Byte> output(extracted->vertex_count *
                gfx::renderer::vertex::VanillaVertex::kSize);
            std::vector<Byte> reference_output(output.size());
            ASSERT_TRUE(gfx::renderer::Render(output, gfx::renderer::VertexKind::kVanilla,
                direct_state, parameters).ok());
            ASSERT_TRUE(gfx::renderer::Render(reference_output, gfx::renderer::VertexKind::kVanilla,
                reference_state, parameters).ok());
            EXPECT_EQ(output, reference_output);
            if (partition == 1) {
                EXPECT_EQ(extracted->vertex_count, 4);
                const auto* vertices = reinterpret_cast<const
                    gfx::renderer::vertex::VanillaVertex*>(output.data());
                EXPECT_EQ(vertices[0].light, parameters.light);
            }
        }
    }
}

#ifdef YSM_X64
INSTANTIATE_TEST_SUITE_P(SupportedSimd, DirectBakedModelTest,
    testing::Values(simd::Type::SSE41, simd::Type::AVX2, simd::Type::AVX512));
#elif defined(YSM_ARM64)
INSTANTIATE_TEST_SUITE_P(SupportedSimd, DirectBakedModelTest, testing::Values(simd::Type::NEON));
#endif

}  // namespace
}  // namespace ysm::test
