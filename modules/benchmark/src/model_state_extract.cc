#include "model_state_extract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <ylt/struct_pb.hpp>

#include "gfx/bake/baked_model.h"
#include "gfx/bake/baked_serializer.h"
#include "cpu.h"
#include "inline.h"
#include "gfx/renderer/model_state.h"
#include "proto/asset/model/data/geo_model.proto.h"
#include "scope_guard.h"

namespace ysm::benchmarking {
namespace {

constexpr size_t kFrameCount = 8;
constexpr float kPoseTolerance = 0.00001f;

using ExtractOutput = gfx::renderer::ModelState::ExtractOutput;
using ExtractKernel = absl::StatusOr<ExtractOutput> (*)(
    gfx::renderer::ModelState&, const std::shared_ptr<gfx::bake::BakedModel>&,
    std::span<const gfx::renderer::BoneAttribute>, size_t);

struct Variant {
    std::string_view isa;
    ExtractKernel extract;
};

using Frames =
    std::array<std::vector<gfx::renderer::BoneAttribute>, kFrameCount>;

YSM_NOINLINE absl::StatusOr<ExtractOutput> ExtractGeneric(
    gfx::renderer::ModelState& state,
    const std::shared_ptr<gfx::bake::BakedModel>& model,
    std::span<const gfx::renderer::BoneAttribute> attributes,
    size_t locator_capacity) {
    return state.Extract(simd::Tag<simd::Type::kNone>{}, model, attributes,
                         locator_capacity);
}

#ifdef YSM_X64

YSM_NOINLINE absl::StatusOr<ExtractOutput> ExtractSse41(
    gfx::renderer::ModelState& state,
    const std::shared_ptr<gfx::bake::BakedModel>& model,
    std::span<const gfx::renderer::BoneAttribute> attributes,
    size_t locator_capacity) {
    return state.Extract(simd::Tag<simd::Type::SSE41>{}, model, attributes,
                         locator_capacity);
}

YSM_NOINLINE absl::StatusOr<ExtractOutput> ExtractAvx2(
    gfx::renderer::ModelState& state,
    const std::shared_ptr<gfx::bake::BakedModel>& model,
    std::span<const gfx::renderer::BoneAttribute> attributes,
    size_t locator_capacity) {
    return state.Extract(simd::Tag<simd::Type::AVX2>{}, model, attributes,
                         locator_capacity);
}

YSM_NOINLINE absl::StatusOr<ExtractOutput> ExtractAvx512(
    gfx::renderer::ModelState& state,
    const std::shared_ptr<gfx::bake::BakedModel>& model,
    std::span<const gfx::renderer::BoneAttribute> attributes,
    size_t locator_capacity) {
    return state.Extract(simd::Tag<simd::Type::AVX512>{}, model, attributes,
                         locator_capacity);
}

std::vector<Variant> SupportedVariants() {
    std::vector<Variant> variants{{"SSE41", ExtractSse41}};
    if (simd::kSupported == simd::Type::AVX2 ||
        simd::kSupported == simd::Type::AVX512) {
        variants.push_back({"AVX2_FMA", ExtractAvx2});
    }
    if (simd::kSupported == simd::Type::AVX512) {
        variants.push_back({"AVX512", ExtractAvx512});
    }
    return variants;
}

#elif defined(YSM_ARM64)

YSM_NOINLINE absl::StatusOr<ExtractOutput> ExtractNeon(
    gfx::renderer::ModelState& state,
    const std::shared_ptr<gfx::bake::BakedModel>& model,
    std::span<const gfx::renderer::BoneAttribute> attributes,
    size_t locator_capacity) {
    return state.Extract(simd::Tag<simd::Type::NEON>{}, model, attributes,
                         locator_capacity);
}

std::vector<Variant> SupportedVariants() {
    return {{"NEON", ExtractNeon}};
}

#endif

size_t AppendBalancedSubtree(gfx::bake::BakedModelBones& bones,
                             size_t logical_index, size_t bone_count,
                             uint32_t parent_index, uint32_t depth) {
    if (logical_index >= bone_count) {
        return bones.list.size();
    }

    const auto bone_index = static_cast<uint32_t>(bones.list.size());
    bones.list.emplace_back();
    auto& bone = bones.list.back();
    bone.parent_index = parent_index;
    bone.depth = depth;
    bone.pivot = {
        static_cast<float>((logical_index * 5) % 17) - 8.0f,
        static_cast<float>((logical_index * 7) % 13) - 6.0f,
        static_cast<float>((logical_index * 11) % 19) - 9.0f,
    };
    bone.solid = true;

    AppendBalancedSubtree(bones, logical_index * 2 + 1, bone_count,
                          bone_index, depth + 1);
    AppendBalancedSubtree(bones, logical_index * 2 + 2, bone_count,
                          bone_index, depth + 1);
    bones.list[bone_index].subtree_end =
        static_cast<uint32_t>(bones.list.size());
    return bone_index;
}

std::shared_ptr<gfx::bake::BakedModel> MakeModel(size_t bone_count) {
    gfx::bake::BakedModelBones bones;
    bones.list.reserve(bone_count);
    AppendBalancedSubtree(bones, 0, bone_count, UINT32_MAX, 0);

    gfx::bake::BakedModelCubes<simd::Width::B128> cubes;
    cubes.cutout_no_culling.resize(bone_count);
    bones.sorted_bone_indices.reserve(bone_count);
    bones.cube_indices_cache.resize(bone_count);
    bones.cube_group_info_cache.resize(bone_count);
    for (size_t bone_index = 0; bone_index < bone_count; ++bone_index) {
        bones.sorted_bone_indices.push_back(
            static_cast<uint16_t>(bone_index));
        bones.cube_indices_cache[bone_index] =
            static_cast<uint32_t>(bone_index);
        bones.cube_group_info_cache[bone_index] = {1, 1, 1};

        auto& group = cubes.cutout_no_culling[bone_index];
        group.bone_index = static_cast<uint32_t>(bone_index);
        group.cube_count = 1;
        group.cube_attr[0].quad_count = 1;
        group.cube_attr[0].quad_count_after_culling = 1;

        auto& partition = bones.list[bone_index].cutout_no_culling;
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

std::shared_ptr<gfx::bake::BakedModel> MakeCacheModel(size_t group_count,
                                                  bool has_pbr) {
    proto::asset::model::data::CubeLegacy cube;
    cube.faceCount = 1;
    cube.pos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    cube.pos_indices = {0, 1, 2, 3};
    cube.uv = {0, 0, 1, 0, 1, 1, 0, 1};
    cube.uv_indices = {0, 1, 2, 3};
    cube.normal = {0, 0, 1};

    proto::asset::model::data::GeoModel source;
    source.bones.resize(1);
    source.bones[0].name = "root";
    source.bones[0].pivot = {0, 0, 0};
    source.bones[0].rotate = {0, 0, 0};
    source.bones[0].cubeCount = static_cast<uint32_t>(group_count * 2);
    source.cubes.cubes_legacy.assign(group_count * 2, cube);
    std::string protobuf;
    struct_pb::to_pb(source, protobuf);

    const std::array<gfx::bake::Pixel, 1> pixels{
        gfx::bake::Pixel{255, 255, 255, 255},
    };
    const gfx::bake::Texture texture(pixels.data(), 1, 1);
    auto model = gfx::bake::BakeModel(StrBuf(protobuf), texture,
                                     {.has_pbr = has_pbr});
    if (!model.ok()) {
        throw std::runtime_error(model.status().ToString());
    }
    return std::shared_ptr<gfx::bake::BakedModel>(std::move(*model));
}

float UnitValue(size_t value) noexcept {
    return static_cast<float>(value % 257) / 256.0f;
}

Frames MakeFrames(size_t bone_count) {
    Frames frames;
    for (size_t frame = 0; frame < frames.size(); ++frame) {
        frames[frame].resize(bone_count);
        for (size_t bone_index = 0; bone_index < bone_count; ++bone_index) {
            auto& attribute = frames[frame][bone_index];
            const float x = UnitValue(bone_index * 17 + frame * 13);
            const float y = UnitValue(bone_index * 29 + frame * 7 + 31);
            const float z = UnitValue(bone_index * 43 + frame * 19 + 73);
            attribute.rotation[0] = (x - 0.5f) * 0.9f;
            attribute.rotation[1] = (y - 0.5f) * 1.3f;
            attribute.rotation[2] = (z - 0.5f) * 2.2f;
            attribute.position[0] = (y - 0.5f) * 4.0f;
            attribute.position[1] = (z - 0.5f) * 3.0f;
            attribute.position[2] = (x - 0.5f) * 2.0f;
            attribute.scale[0] = 1.0f;
            attribute.scale[1] = 1.0f;
            attribute.scale[2] = 1.0f;
            if (bone_index != 0 && bone_index % 17 == 0) {
                attribute.locator_sequence =
                    static_cast<float>(bone_index / 17);
            }
        }
    }
    return frames;
}

struct Fixture {
    explicit Fixture(size_t bone_count)
        : model(MakeModel(bone_count)),
          frames(MakeFrames(bone_count)) {}

    std::shared_ptr<gfx::bake::BakedModel> model;
    Frames frames;
    gfx::renderer::ModelState state;
};

float MaxPoseDifference(const gfx::renderer::BonePose& left,
                        const gfx::renderer::BonePose& right) noexcept {
    float difference = 0.0f;
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row) {
            difference = std::max(
                difference,
                std::abs(left.pose[column][row] - right.pose[column][row]));
        }
    }
    for (size_t column = 0; column < 3; ++column) {
        for (size_t row = 0; row < 3; ++row) {
            difference = std::max(
                difference,
                std::abs(left.normal[column][row] -
                         right.normal[column][row]));
        }
    }
    difference =
        std::max(difference,
                 std::abs(left.tangent_orientation -
                          right.tangent_orientation));
    difference = std::max(
        difference, std::abs(left.normal_scale - right.normal_scale));
    return difference;
}

bool PrepareSteadyState(benchmark::State& benchmark_state,
                        ExtractKernel extract, Fixture& fixture) {
    const auto first = extract(fixture.state, fixture.model, fixture.frames[0],
                                fixture.frames[0].size());
    if (!first.ok()) {
        const std::string error = first.status().ToString();
        benchmark_state.SkipWithError(error.c_str());
        return false;
    }
    const auto second = extract(fixture.state, fixture.model, fixture.frames[1],
                                 fixture.frames[1].size());
    if (!second.ok()) {
        const std::string error = second.status().ToString();
        benchmark_state.SkipWithError(error.c_str());
        return false;
    }
    if (second->schedule_updated) {
        benchmark_state.SkipWithError(
            "ModelState Extract did not reuse its render schedule");
        return false;
    }
    return true;
}

void SteadyStateExtract(benchmark::State& benchmark_state,
                        ExtractKernel extract, size_t bone_count) {
    Fixture fixture(bone_count);
    if (!PrepareSteadyState(benchmark_state, extract, fixture)) {
        return;
    }

    size_t frame = 2;
    for (auto _ : benchmark_state) {
        auto output =
            extract(fixture.state, fixture.model,
                     fixture.frames[frame % fixture.frames.size()],
                     fixture.frames[0].size());
        if (!output.ok()) {
            const std::string error = output.status().ToString();
            benchmark_state.SkipWithError(error.c_str());
            break;
        }
        benchmark::DoNotOptimize(output->vertex_count);
        benchmark::DoNotOptimize(
            fixture.state.PoseView().bone_poses.back().pose[0][0]);
        ++frame;
    }
    benchmark_state.SetItemsProcessed(benchmark_state.iterations() *
                                      bone_count);
}

void SerializeBakedModel(benchmark::State& benchmark_state,
                         size_t group_count, simd::Type type, bool has_pbr) {
    const auto previous_type = simd::kSupported;
    const auto restore = ScopeGuard([&] { simd::Mock(previous_type); });
    simd::Mock(type);
    const auto model = MakeCacheModel(group_count, has_pbr);
    for (auto _ : benchmark_state) {
        auto serialized = gfx::bake::SerializeBakedModel(*model);
        benchmark::DoNotOptimize(serialized.data());
        benchmark::ClobberMemory();
    }
    const auto serialized = gfx::bake::SerializeBakedModel(*model);
    benchmark_state.SetBytesProcessed(benchmark_state.iterations() *
                                      serialized.size());
    benchmark_state.counters["payload_bytes"] =
        static_cast<double>(serialized.size());
}

void DeserializeBakedModel(benchmark::State& benchmark_state,
                           size_t group_count, simd::Type type, bool has_pbr) {
    const auto previous_type = simd::kSupported;
    const auto restore = ScopeGuard([&] { simd::Mock(previous_type); });
    simd::Mock(type);
    const auto model = MakeCacheModel(group_count, has_pbr);
    const auto serialized = gfx::bake::SerializeBakedModel(*model);
    for (auto _ : benchmark_state) {
        auto restored = gfx::bake::ReadBakedModel(serialized);
        if (!restored.ok()) {
            const std::string error = restored.status().ToString();
            benchmark_state.SkipWithError(error.c_str());
            return;
        }
        benchmark::DoNotOptimize(restored->get());
    }
    benchmark_state.SetBytesProcessed(benchmark_state.iterations() *
                                      serialized.size());
    benchmark_state.counters["payload_bytes"] =
        static_cast<double>(serialized.size());
}

}  // namespace

bool ValidateModelStateExtractBenchmarks() {
    constexpr size_t kValidationBoneCount = 31;
    Fixture reference(kValidationBoneCount);
    const auto expected =
        ExtractGeneric(reference.state, reference.model, reference.frames[3],
                       reference.frames[3].size());
    if (!expected.ok()) {
        std::fprintf(stderr,
                     "ModelState Extract generic validation failed: %s\n",
                     expected.status().ToString().c_str());
        return false;
    }

    for (const auto& variant : SupportedVariants()) {
        Fixture actual(kValidationBoneCount);
        const auto output =
            variant.extract(actual.state, actual.model, actual.frames[3],
                            actual.frames[3].size());
        if (!output.ok()) {
            std::fprintf(stderr,
                         "ModelState Extract %.*s validation failed: %s\n",
                         static_cast<int>(variant.isa.size()),
                         variant.isa.data(), output.status().ToString().c_str());
            return false;
        }
        if (output->vertex_count != expected->vertex_count ||
            output->locator_count != expected->locator_count) {
            std::fprintf(stderr,
                         "ModelState Extract %.*s validation returned "
                         "different counts\n",
                         static_cast<int>(variant.isa.size()),
                         variant.isa.data());
            return false;
        }
        for (size_t bone_index = 0; bone_index < kValidationBoneCount;
             ++bone_index) {
            const float difference = MaxPoseDifference(
                reference.state.PoseView().bone_poses[bone_index],
                actual.state.PoseView().bone_poses[bone_index]);
            if (difference > kPoseTolerance) {
                std::fprintf(stderr,
                             "ModelState Extract %.*s validation failed at "
                             "bone %zu: max abs difference %.9g\n",
                             static_cast<int>(variant.isa.size()),
                             variant.isa.data(), bone_index,
                             static_cast<double>(difference));
                return false;
            }
        }
    }
    return true;
}

void RegisterModelStateExtractBenchmarks() {
    benchmark::AddCustomContext("model_state_extract_fixture",
                                "balanced hierarchy, one visible group per "
                                "bone, uniform scale, schedule reused");
    benchmark::AddCustomContext("model_state_extract_input_frames",
                                std::to_string(kFrameCount));
    constexpr size_t kBoneCounts[]{32, 96};
    for (const auto& variant : SupportedVariants()) {
        for (const size_t bone_count : kBoneCounts) {
            std::string name = "ModelStateExtract/SteadyState/";
            name += std::to_string(bone_count);
            name += "Bones/";
            name += variant.isa;
            benchmark::RegisterBenchmark(name, SteadyStateExtract,
                                         variant.extract, bone_count)
                ->UseRealTime()
                ->Unit(benchmark::kNanosecond);
        }
    }
    std::vector<simd::Type> cache_types{simd::kSupported};
#ifdef YSM_X64
    if (simd::kSupported != simd::Type::kNone) {
        cache_types = {simd::Type::SSE41};
    }
    if (simd::kSupported == simd::Type::AVX2 ||
        simd::kSupported == simd::Type::AVX512) {
        cache_types.push_back(simd::Type::AVX2);
    }
    if (simd::kSupported == simd::Type::AVX512) {
        cache_types.push_back(simd::Type::AVX512);
    }
#endif
    constexpr size_t kSerializationGroupCount = 96;
    for (const auto type : cache_types) {
        for (const bool has_pbr : {false, true}) {
            const auto suffix = std::string(magic_enum::enum_name(type)) +
                                (has_pbr ? "/Pbr" : "/NonPbr");
            const auto serialize_name = "BakedModelCache/Serialize/" + suffix;
            const auto deserialize_name =
                "BakedModelCache/Deserialize/" + suffix;
            benchmark::RegisterBenchmark(serialize_name, SerializeBakedModel,
                                         kSerializationGroupCount, type, has_pbr)
                ->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
            benchmark::RegisterBenchmark(deserialize_name,
                                         DeserializeBakedModel,
                                         kSerializationGroupCount, type, has_pbr)
                ->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
        }
    }
}

}  // namespace ysm::benchmarking
