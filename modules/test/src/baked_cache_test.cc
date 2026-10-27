#include <array>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "cpu.h"
#include "gfx/bake/baked_model.h"
#include "gfx/bake/baked_serializer.h"
#include "scope_guard.h"

namespace ysm::test {
namespace {

template <typename Group>
Group MakeCacheGroup(uint32_t bone_index) {
    Group group;
    group.bone_index = bone_index;
    group.cube_count = bone_index == 0 ? 1 : Group::kCubeGroupCapacity;
    for (size_t cube = 0; cube < group.cube_count; ++cube) {
        auto& attr = group.cube_attr[cube];
        attr.quad_count = bone_index == 0 ? 1 : 6;
        attr.quad_count_after_culling = attr.quad_count - 1;
        for (size_t quad = 0; quad < attr.quad_count; ++quad) {
            const auto index = Group::GetQuadAttrIndex(cube, quad);
            group.normal[2][index] = 1.0f;
            group.plane_d[index] = 2.5f;
            group.winding_sign[index] = -1.0f;
            group.tangent[0][index] = 1.0f;
            group.tangent[3][index] = -1.0f;
            if constexpr (Group::kTranslucent) {
                group.center[0][index] = 0.5f;
            }
            for (size_t vertex = 0; vertex < 4; ++vertex) {
                const auto vertex_index = cube * 8 + vertex;
                group.vertex_index[vertex][index] = vertex_index;
                group.pos[0][vertex_index] = static_cast<float>(vertex);
                group.pos[1][vertex_index] = static_cast<float>(bone_index);
                auto& uv = attr.quad_attr[quad].uv[vertex];
                uv[0] = static_cast<float>(quad + vertex) / 16.0f;
                uv[1] = static_cast<float>(cube + vertex) / 8.0f;
            }
            attr.quad_attr[quad].mid_uv[0] = 0.25f;
            attr.quad_attr[quad].mid_uv[1] = 0.75f;
        }
    }
    return group;
}

enum class CacheFault { kNone, kGroupCapacity, kPartitionIndex };

template <simd::Width kWidth>
gfx::bake::BakedModel MakeCacheModel(bool has_pbr, CacheFault fault = CacheFault::kNone) {
    using CutoutGroup = gfx::bake::CubeGroup<kWidth, false>;
    using TranslucentGroup = gfx::bake::CubeGroup<kWidth, true>;
    gfx::bake::BakedModelCubes<kWidth> cubes;
    cubes.cutout = {MakeCacheGroup<CutoutGroup>(0),
                    MakeCacheGroup<CutoutGroup>(1)};
    cubes.cutout_no_culling = cubes.cutout;
    cubes.translucent = {MakeCacheGroup<TranslucentGroup>(0),
                         MakeCacheGroup<TranslucentGroup>(1)};
    cubes.translucent_culling = cubes.translucent;
    if (fault == CacheFault::kGroupCapacity) {
        cubes.cutout[0].cube_count = CutoutGroup::kCubeGroupCapacity + 1;
    }

    gfx::bake::BakedModelBones bones;
    bones.list.resize(2);
    bones.sorted_bone_indices = {1, 0};
    bones.cube_indices_cache = {0, 0, 0, 0, 1, 1, 1, 1};
    if (fault == CacheFault::kPartitionIndex) {
        bones.cube_indices_cache[0] = 2;
    }
    bones.cube_group_info_cache.resize(8);
    for (size_t index = 0; index < bones.list.size(); ++index) {
        auto& bone = bones.list[index];
        bone.pivot = {1.0f, -2.0f, static_cast<float>(index)};
        bone.parent_index = index == 0 ? UINT32_MAX : 0;
        bone.subtree_end = 2;
        bone.depth = static_cast<uint32_t>(index);
        bone.solid = index == 1;
        const std::array partitions{&bone.cutout, &bone.cutout_no_culling,
                                    &bone.translucent,
                                    &bone.translucent_culling};
        for (size_t partition = 0; partition < partitions.size(); ++partition) {
            const auto offset = index * partitions.size() + partition;
            partitions[partition]->cube_indices =
                std::span(bones.cube_indices_cache).subspan(offset, 1);
        }
    }
    return gfx::bake::BakedModel({.gui_no_shadow = true, .has_pbr = has_pbr},
                                std::move(bones), std::move(cubes));
}

using CacheCase = std::tuple<simd::Type, bool>;
class BakedCacheTest : public ::testing::TestWithParam<CacheCase> {};

TEST_P(BakedCacheTest, PreservesEncodedFieldsAndRebuildsPartitions) {
    InitCpuInfo();
    const auto previous_type = simd::kSupported;
    const auto restore = ScopeGuard([&] { simd::Mock(previous_type); });
    const auto [type, has_pbr] = GetParam();
    simd::Mock(type);
    magic_enum::enum_switch(
        [&](auto isa) {
            constexpr auto kWidth = simd::width<isa>();
            const auto source = MakeCacheModel<kWidth>(has_pbr);
            const auto serialized = gfx::bake::SerializeBakedModel(source);
            const auto restored = gfx::bake::ReadBakedModel(serialized);
            ASSERT_TRUE(restored.ok()) << restored.status();
            const auto reserialized =
                gfx::bake::SerializeBakedModel(**restored);
            ASSERT_EQ(serialized.size(), reserialized.size());
            EXPECT_EQ(std::memcmp(serialized.data(), reserialized.data(),
                                  serialized.size()),
                      0);
            const auto& bone = (*restored)->Bones().list[1];
            EXPECT_TRUE(bone.solid);
            EXPECT_EQ(bone.depth, 1);
            EXPECT_EQ(bone.cutout.cube_indices.size(), 1);
            ASSERT_EQ(bone.cutout.cube_group_info.size(), 1);
            EXPECT_EQ(bone.cutout.cube_group_info[0].cube_count,
                      (gfx::bake::CubeGroup<kWidth, false>::kCubeGroupCapacity));
            EXPECT_GT(bone.cutout.full_vertex_count, 0);
            const auto& group =
                (*restored)->template Cubes<kWidth>().translucent[1];
            EXPECT_FLOAT_EQ(group.tangent[0][0], has_pbr ? 1.0f : 0.0f);
            EXPECT_FLOAT_EQ(group.tangent[3][0], has_pbr ? -1.0f : 0.0f);
        },
        type);
}

TEST_P(BakedCacheTest, CapacityAndSerializationBoundsCoverEveryBacking) {
    InitCpuInfo();
    const auto previous_type = simd::kSupported;
    const auto restore = ScopeGuard([&] { simd::Mock(previous_type); });
    const auto [type, has_pbr] = GetParam();
    simd::Mock(type);
    magic_enum::enum_switch(
        [&](auto isa) {
            constexpr auto kWidth = simd::width<isa>();
            const auto model = MakeCacheModel<kWidth>(has_pbr);
            const auto& bones = model.Bones();
            const auto& cubes = model.template Cubes<kWidth>();
            const size_t expected =
                sizeof(model) + sizeof(cubes) +
                bones.list.capacity() *
                    sizeof(gfx::bake::BakedModelBones::BoneInfo) +
                bones.sorted_bone_indices.capacity() * sizeof(uint16_t) +
                bones.cube_indices_cache.capacity() * sizeof(uint32_t) +
                bones.cube_group_info_cache.capacity() *
                    sizeof(gfx::bake::BakedModelBones::CubeGroupInfo) +
                (cubes.cutout.capacity() + cubes.cutout_no_culling.capacity()) *
                    sizeof(gfx::bake::CubeGroup<kWidth, false>) +
                (cubes.translucent.capacity() +
                 cubes.translucent_culling.capacity()) *
                    sizeof(gfx::bake::CubeGroup<kWidth, true>);
            EXPECT_EQ(model.CapacityBytes(), expected);
            const auto output = gfx::bake::SerializeBakedModel(model);
            EXPECT_EQ(output.size(),
                      gfx::bake::BakedModelSerializationBytes(model));
            EXPECT_EQ(output.capacity(), output.size());
        },
        type);
}

TEST_P(BakedCacheTest, RejectsIncompleteTrailingAndInvalidPartitionData) {
    InitCpuInfo();
    const auto previous_type = simd::kSupported;
    const auto restore = ScopeGuard([&] { simd::Mock(previous_type); });
    const auto [type, has_pbr] = GetParam();
    simd::Mock(type);
    magic_enum::enum_switch(
        [&](auto isa) {
            constexpr auto kWidth = simd::width<isa>();
            const auto source = MakeCacheModel<kWidth>(has_pbr);
            auto serialized = gfx::bake::SerializeBakedModel(source);
            for (size_t size = 0; size < serialized.size(); ++size) {
                ASSERT_FALSE(gfx::bake::ReadBakedModel(
                                 BufferViewR{serialized.data(), size})
                                 .ok())
                    << "Accepted prefix of " << size << " bytes";
            }
            serialized.resize(serialized.size() + 1);
            serialized[serialized.size() - 1] = 0;
            for (const auto fault : {CacheFault::kGroupCapacity, CacheFault::kPartitionIndex}) {
                auto invalid = MakeCacheModel<kWidth>(has_pbr, fault);
                EXPECT_FALSE(gfx::bake::ReadBakedModel(
                    gfx::bake::SerializeBakedModel(invalid)).ok());
            }
            EXPECT_FALSE(gfx::bake::ReadBakedModel(serialized).ok());
        },
        type);
}

#ifdef YSM_X64
constexpr std::array kCacheTypes{simd::Type::SSE41, simd::Type::AVX2,
                                 simd::Type::AVX512};
#else
constexpr std::array kCacheTypes{simd::Type::NEON};
#endif

INSTANTIATE_TEST_SUITE_P(
    SupportedWidths, BakedCacheTest,
    ::testing::Combine(::testing::ValuesIn(kCacheTypes), ::testing::Bool()));

}  // namespace
}  // namespace ysm::test
