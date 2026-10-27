#include "gfx/bake/baked_serializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include <ylt/struct_pack.hpp>

namespace ysm::gfx::bake {
struct BakedModelSerdeAccess {
    static BakedModelInfo& Info(BakedModel& model) { return model.info_; }

    static BakedModelBones& Bones(BakedModel& model) { return model.bones_; }

    template <simd::Width kWidth>
    static BakedModelCubes<kWidth>& Cubes(BakedModel& model) {
        return std::any_cast<BakedModelCubes<kWidth>&>(model.cubes_);
    }
};

namespace serde {
struct BakedModelHeader {
    uint8_t vec_bytes = 0;
};

template <simd::Width kWidth>
struct BakedModelBody {
    BakedModel& model;
};

template <simd::Width kWidth>
constexpr std::string_view sp_set_type_name(BakedModelBody<kWidth>*) {
    if constexpr (kWidth == simd::Width::B128) {
        return "ysm.BakedModelBody.128";
    } else if constexpr (kWidth == simd::Width::B256) {
        return "ysm.BakedModelBody.256";
    } else {
        return "ysm.BakedModelBody.512";
    }
}

using BonePartitionInfo = BakedModelBones::BonePartitionInfo;
using BoneInfo = BakedModelBones::BoneInfo;

// Reflect borrowed fields so runtime layouts and array bulk copies stay intact.
template <typename Bone>
struct BoneMetadataView {
    Bone& value;
    YLT_REFL(BoneMetadataView, value.pivot, value.parent_index,
             value.subtree_end, value.depth, value.solid);
};

template <typename Cube>
struct CubeCountsView {
    Cube& value;
    YLT_REFL(CubeCountsView, value.quad_count, value.quad_count_after_culling);
};

template <typename Quad>
struct QuadUvView {
    Quad& value;
    YLT_REFL(QuadUvView, value.uv, value.mid_uv);
};

template <typename Reader, typename View>
struct_pack::err_code ReadFields(Reader& reader, View view) {
    return View::refl_visit_members(view, [&](auto&... fields) {
        struct_pack::err_code ec{};
        ([&] {
            if (!ec) {
                ec = struct_pack::read(reader, fields);
            }
        }(), ...);
        return ec;
    });
}

std::array<const BonePartitionInfo*, 4> Partitions(const BoneInfo& bone) {
    return {&bone.cutout, &bone.cutout_no_culling, &bone.translucent,
            &bone.translucent_culling};
}

std::array<BonePartitionInfo*, 4> Partitions(BoneInfo& bone) {
    return {&bone.cutout, &bone.cutout_no_culling, &bone.translucent,
            &bone.translucent_culling};
}

uint64_t IndexCount(const BakedModelBones& bones) {
    uint64_t result = 0;
    for (const auto& bone : bones.list) {
        for (const auto* partition : Partitions(bone)) {
            result += partition->cube_indices.size();
        }
    }
    return result;
}

template <simd::Width kWidth, bool kTranslucent>
size_t GroupSize(const CubeGroup<kWidth, kTranslucent>& group, bool has_pbr) {
    using Group = CubeGroup<kWidth, kTranslucent>;
    size_t result = struct_pack::get_write_size(group.pos) +
                    struct_pack::get_write_size(group.normal) +
                    struct_pack::get_write_size(group.plane_d) +
                    struct_pack::get_write_size(group.winding_sign) +
                    struct_pack::get_write_size(group.vertex_index) +
                    struct_pack::get_write_size(group.cube_count) +
                    struct_pack::get_write_size(group.bone_index);
    if constexpr (kTranslucent) {
        result += struct_pack::get_write_size(group.center);
    }
    if (has_pbr) {
        result += struct_pack::get_write_size(group.tangent);
    }
    const auto cube_count =
        std::min<size_t>(group.cube_count, Group::kCubeGroupCapacity);
    for (size_t cube_index = 0; cube_index < cube_count; ++cube_index) {
        const auto& cube = group.cube_attr[cube_index];
        result += struct_pack::get_write_size(CubeCountsView{cube});
        const auto quad_count = std::min<size_t>(cube.quad_count, 6);
        for (size_t quad = 0; quad < quad_count; ++quad) {
            const auto& attr = cube.quad_attr[quad];
            result += struct_pack::get_write_size(QuadUvView{attr});
        }
    }
    return result;
}

template <typename Writer, simd::Width kWidth, bool kTranslucent>
void WriteGroup(Writer& writer, const CubeGroup<kWidth, kTranslucent>& group,
                bool has_pbr) {
    using Group = CubeGroup<kWidth, kTranslucent>;
    struct_pack::write(writer, group.pos);
    if constexpr (kTranslucent) {
        struct_pack::write(writer, group.center);
    }
    struct_pack::write(writer, group.normal);
    struct_pack::write(writer, group.plane_d);
    struct_pack::write(writer, group.winding_sign);
    if (has_pbr) {
        struct_pack::write(writer, group.tangent);
    }
    struct_pack::write(writer, group.vertex_index);
    struct_pack::write(writer, group.cube_count);
    struct_pack::write(writer, group.bone_index);
    const auto cube_count =
        std::min<size_t>(group.cube_count, Group::kCubeGroupCapacity);
    for (size_t cube_index = 0; cube_index < cube_count; ++cube_index) {
        const auto& cube = group.cube_attr[cube_index];
        struct_pack::write(writer, CubeCountsView{cube});
        const auto quad_count = std::min<size_t>(cube.quad_count, 6);
        for (size_t quad = 0; quad < quad_count; ++quad) {
            const auto& attr = cube.quad_attr[quad];
            struct_pack::write(writer, QuadUvView{attr});
        }
    }
}

template <typename Reader, simd::Width kWidth, bool kTranslucent>
struct_pack::err_code ReadGroup(Reader& reader,
                                CubeGroup<kWidth, kTranslucent>& group,
                                bool has_pbr) {
    using Group = CubeGroup<kWidth, kTranslucent>;
    if (auto ec = struct_pack::read(reader, group.pos); ec) {
        return ec;
    }
    if constexpr (kTranslucent) {
        if (auto ec = struct_pack::read(reader, group.center); ec) {
            return ec;
        }
    }
    if (auto ec = struct_pack::read(reader, group.normal); ec) {
        return ec;
    }
    if (auto ec = struct_pack::read(reader, group.plane_d); ec) {
        return ec;
    }
    if (auto ec = struct_pack::read(reader, group.winding_sign); ec) {
        return ec;
    }
    if (has_pbr) {
        if (auto ec = struct_pack::read(reader, group.tangent); ec) {
            return ec;
        }
    }
    if (auto ec = struct_pack::read(reader, group.vertex_index); ec) {
        return ec;
    }
    if (auto ec = struct_pack::read(reader, group.cube_count); ec) {
        return ec;
    }
    if (auto ec = struct_pack::read(reader, group.bone_index); ec) {
        return ec;
    }
    if (group.cube_count == 0 ||
        group.cube_count > Group::kCubeGroupCapacity) {
        return struct_pack::errc::invalid_buffer;
    }
    for (size_t cube_index = 0; cube_index < group.cube_count; ++cube_index) {
        auto& cube = group.cube_attr[cube_index];
        if (auto ec = ReadFields(reader, CubeCountsView{cube}); ec) {
            return ec;
        }
        if (cube.quad_count == 0 || cube.quad_count > 6 ||
            cube.quad_count_after_culling > cube.quad_count) {
            return struct_pack::errc::invalid_buffer;
        }
        for (size_t quad = 0; quad < cube.quad_count; ++quad) {
            auto& attr = cube.quad_attr[quad];
            if (auto ec = ReadFields(reader, QuadUvView{attr}); ec) {
                return ec;
            }
        }
    }
    return {};
}

template <typename Group>
size_t GroupsSize(const std::vector<Group>& groups, bool has_pbr) {
    size_t result =
        struct_pack::get_write_size(static_cast<uint64_t>(groups.size()));
    for (const auto& group : groups) {
        result += GroupSize(group, has_pbr);
    }
    return result;
}

template <typename Writer, typename Group>
void WriteGroups(Writer& writer, const std::vector<Group>& groups,
                 bool has_pbr) {
    struct_pack::write(writer, static_cast<uint64_t>(groups.size()));
    for (const auto& group : groups) {
        WriteGroup(writer, group, has_pbr);
    }
}

template <typename Reader, typename Group>
struct_pack::err_code ReadGroups(Reader& reader, std::vector<Group>& groups,
                                 bool has_pbr) {
    uint64_t size = 0;
    if (auto ec = struct_pack::read(reader, size); ec) {
        return ec;
    }
    if (size > BufferManaged::kMaxSize / sizeof(Group)) {
        return struct_pack::errc::invalid_buffer;
    }
    groups.resize(static_cast<size_t>(size));
    for (auto& group : groups) {
        if (auto ec = ReadGroup(reader, group, has_pbr); ec) {
            return ec;
        }
    }
    return {};
}

template <simd::Width kWidth>
size_t CubesSize(const BakedModelCubes<kWidth>& cubes, bool has_pbr) {
    return GroupsSize(cubes.cutout, has_pbr) +
           GroupsSize(cubes.cutout_no_culling, has_pbr) +
           GroupsSize(cubes.translucent, has_pbr) +
           GroupsSize(cubes.translucent_culling, has_pbr);
}

template <typename Writer, simd::Width kWidth>
void WriteCubes(Writer& writer, const BakedModelCubes<kWidth>& cubes,
                bool has_pbr) {
    WriteGroups(writer, cubes.cutout, has_pbr);
    WriteGroups(writer, cubes.cutout_no_culling, has_pbr);
    WriteGroups(writer, cubes.translucent, has_pbr);
    WriteGroups(writer, cubes.translucent_culling, has_pbr);
}

template <typename Reader, simd::Width kWidth>
struct_pack::err_code ReadCubes(Reader& reader,
                                BakedModelCubes<kWidth>& cubes,
                                bool has_pbr) {
    if (auto ec = ReadGroups(reader, cubes.cutout, has_pbr); ec) {
        return ec;
    }
    if (auto ec = ReadGroups(reader, cubes.cutout_no_culling, has_pbr); ec) {
        return ec;
    }
    if (auto ec = ReadGroups(reader, cubes.translucent, has_pbr); ec) {
        return ec;
    }
    return ReadGroups(reader, cubes.translucent_culling, has_pbr);
}

template <simd::Width kWidth>
size_t sp_get_needed_size(const BakedModelBody<kWidth>& body) {
    auto& model = body.model;
    const auto& info = BakedModelSerdeAccess::Info(model);
    const auto& bones = BakedModelSerdeAccess::Bones(model);
    const auto& cubes = BakedModelSerdeAccess::Cubes<kWidth>(model);
    const auto bone_count = static_cast<uint64_t>(bones.list.size());
    const auto index_count = IndexCount(bones);

    size_t result = struct_pack::get_write_size(info) +
                    CubesSize(cubes, info.has_pbr) +
                    struct_pack::get_write_size(bones.sorted_bone_indices) +
                    struct_pack::get_write_size(bone_count) +
                    struct_pack::get_write_size(index_count);
    for (const auto& bone : bones.list) {
        result += struct_pack::get_write_size(BoneMetadataView{bone});
        for (const auto* partition : Partitions(bone)) {
            const auto size =
                static_cast<uint64_t>(partition->cube_indices.size());
            result += struct_pack::get_write_size(size) +
                      struct_pack::get_write_size(
                          partition->cube_indices.data(),
                          partition->cube_indices.size());
        }
    }
    return result;
}

template <typename Writer, simd::Width kWidth>
void sp_serialize_to(Writer& writer, const BakedModelBody<kWidth>& body) {
    auto& model = body.model;
    const auto& info = BakedModelSerdeAccess::Info(model);
    const auto& bones = BakedModelSerdeAccess::Bones(model);
    const auto& cubes = BakedModelSerdeAccess::Cubes<kWidth>(model);
    const auto bone_count = static_cast<uint64_t>(bones.list.size());
    const auto index_count = IndexCount(bones);

    struct_pack::write(writer, info);
    WriteCubes(writer, cubes, info.has_pbr);
    struct_pack::write(writer, bones.sorted_bone_indices);
    struct_pack::write(writer, bone_count);
    struct_pack::write(writer, index_count);
    for (const auto& bone : bones.list) {
        struct_pack::write(writer, BoneMetadataView{bone});
        for (const auto* partition : Partitions(bone)) {
            const auto size =
                static_cast<uint64_t>(partition->cube_indices.size());
            struct_pack::write(writer, size);
            struct_pack::write(writer, partition->cube_indices.data(),
                               partition->cube_indices.size());
        }
    }
}

template <typename Reader, typename T>
struct_pack::err_code Read(Reader& reader, T& value) {
    return struct_pack::read(reader, value);
}

template <typename Reader, simd::Width kWidth>
struct_pack::err_code sp_deserialize_to(Reader& reader,
                                        BakedModelBody<kWidth>& body) {
    auto& model = body.model;
    auto& info = BakedModelSerdeAccess::Info(model);
    auto& bones = BakedModelSerdeAccess::Bones(model);
    auto& cubes = BakedModelSerdeAccess::Cubes<kWidth>(model);
    uint64_t bone_count = 0;
    uint64_t index_count = 0;

    if (auto ec = Read(reader, info); ec) {
        return ec;
    }
    if (auto ec = ReadCubes(reader, cubes, info.has_pbr); ec) {
        return ec;
    }
    if (auto ec = Read(reader, bones.sorted_bone_indices); ec) {
        return ec;
    }
    if (auto ec = Read(reader, bone_count); ec) {
        return ec;
    }
    if (auto ec = Read(reader, index_count); ec) {
        return ec;
    }
    if (bone_count > std::numeric_limits<uint32_t>::max() ||
        index_count > BufferManaged::kMaxSize / sizeof(uint32_t)) {
        return struct_pack::errc::invalid_buffer;
    }

    constexpr BoneInfo kEmptyBone{};
    constexpr size_t kMinBoneBytes =
        struct_pack::get_write_size(BoneMetadataView{kEmptyBone}) +
        4 * sizeof(uint64_t);
    if constexpr (struct_pack::checkable_reader_t<Reader>) {
        if (bone_count > std::numeric_limits<size_t>::max() / kMinBoneBytes ||
            !reader.check(static_cast<size_t>(bone_count) * kMinBoneBytes +
                          static_cast<size_t>(index_count) * sizeof(uint32_t))) {
            return struct_pack::errc::no_buffer_space;
        }
    }

    bones.list.clear();
    bones.cube_indices_cache.clear();
    bones.cube_group_info_cache.clear();
    bones.list.resize(static_cast<size_t>(bone_count));
    bones.cube_indices_cache.resize(static_cast<size_t>(index_count));
    bones.cube_group_info_cache.resize(static_cast<size_t>(index_count));
    auto indices = std::span(bones.cube_indices_cache);
    auto group_info = std::span(bones.cube_group_info_cache);
    size_t offset = 0;
    for (auto& bone : bones.list) {
        if (auto ec = ReadFields(reader, BoneMetadataView{bone}); ec) {
            return ec;
        }
        for (auto* partition : Partitions(bone)) {
            uint64_t partition_size = 0;
            if (auto ec = Read(reader, partition_size); ec) {
                return ec;
            }
            if (partition_size > index_count - offset) {
                return struct_pack::errc::invalid_buffer;
            }
            const auto size = static_cast<size_t>(partition_size);
            partition->cube_indices = indices.subspan(offset, size);
            partition->cube_group_info = group_info.subspan(offset, size);
            if (auto ec = struct_pack::read(
                    reader, bones.cube_indices_cache.data() + offset, size);
                ec) {
                return ec;
            }
            offset += size;
        }
    }
    if (offset != index_count) {
        return struct_pack::errc::invalid_buffer;
    }
    return {};
}
}  // namespace serde

namespace {
absl::Status Corrupt() { return DataCorruption(); }

template <simd::Width kWidth, bool kTranslucent>
absl::Status ValidateGroups(
    const std::vector<CubeGroup<kWidth, kTranslucent>>& groups,
    size_t bone_count) {
    using Group = CubeGroup<kWidth, kTranslucent>;
    for (const auto& group : groups) {
        if (group.cube_count == 0 ||
            group.cube_count > Group::kCubeGroupCapacity ||
            group.bone_index >= bone_count) {
            return Corrupt();
        }
        for (size_t cube_index = 0; cube_index < group.cube_count;
             ++cube_index) {
            const auto& cube = group.cube_attr[cube_index];
            if (cube.quad_count == 0 || cube.quad_count > 6 ||
                cube.quad_count_after_culling > cube.quad_count) {
                return Corrupt();
            }
            for (size_t quad = 0; quad < cube.quad_count; ++quad) {
                const auto attr_index =
                    Group::GetQuadAttrIndex(cube_index, quad);
                for (size_t vertex = 0; vertex < 4; ++vertex) {
                    const auto vertex_index =
                        group.vertex_index[vertex][attr_index];
                    if (vertex_index < cube_index * 8 ||
                        vertex_index >= (cube_index + 1) * 8) {
                        return Corrupt();
                    }
                }
            }
        }
    }
    return OkStatus();
}

template <typename Group>
absl::Status ValidatePartition(
    BakedModelBones::BonePartitionInfo& partition,
    const std::vector<Group>& groups, size_t bone_index,
    std::span<BakedModelBones::CubeGroupInfo> group_info_cache,
    size_t& cache_offset) {
    if (cache_offset > group_info_cache.size() ||
        partition.cube_group_info.size() != partition.cube_indices.size() ||
        partition.cube_indices.size() >
            group_info_cache.size() - cache_offset) {
        return Corrupt();
    }
    uint64_t cube_count = 0;
    uint64_t quad_count = 0;
    uint64_t culled_quad_count = 0;
    for (size_t position = 0; position < partition.cube_indices.size();
         ++position) {
        const auto index = partition.cube_indices[position];
        if (index >= groups.size() || groups[index].bone_index != bone_index ||
            (position != 0 && index <= partition.cube_indices[position - 1])) {
            return Corrupt();
        }
        const auto& group = groups[index];
        cube_count += group.cube_count;
        for (size_t cube = 0; cube < group.cube_count; ++cube) {
            quad_count += group.cube_attr[cube].quad_count;
            culled_quad_count +=
                group.cube_attr[cube].quad_count_after_culling;
        }
        if (cube_count > std::numeric_limits<uint32_t>::max() ||
            quad_count > std::numeric_limits<uint32_t>::max() ||
            culled_quad_count > std::numeric_limits<uint32_t>::max()) {
            return Corrupt();
        }
        group_info_cache[cache_offset + position] = {
            static_cast<uint32_t>(cube_count),
            static_cast<uint32_t>(quad_count),
            static_cast<uint32_t>(culled_quad_count),
        };
    }
    partition.cube_count = static_cast<uint32_t>(cube_count);
    partition.full_vertex_count = static_cast<uint32_t>(quad_count * 4);
    partition.culling_vertex_count =
        static_cast<uint32_t>(culled_quad_count * 4);
    cache_offset += partition.cube_indices.size();
    return OkStatus();
}

absl::Status ValidateHierarchy(BakedModelBones& bones) {
    const size_t bone_count = bones.list.size();
    if (bones.sorted_bone_indices.size() != bone_count) {
        return Corrupt();
    }
    std::vector<bool> seen_original_indices(bone_count);
    for (const auto original : bones.sorted_bone_indices) {
        if (original >= bone_count || seen_original_indices[original]) {
            return Corrupt();
        }
        seen_original_indices[original] = true;
    }

    for (size_t index = 0; index < bone_count; ++index) {
        const auto& bone = bones.list[index];
        if (bone.subtree_end <= index || bone.subtree_end > bone_count ||
            (bone.parent_index != UINT32_MAX && bone.parent_index >= index)) {
            return Corrupt();
        }
        const auto expected_depth =
            bone.parent_index == UINT32_MAX
                ? 0ULL
                : static_cast<uint64_t>(bones.list[bone.parent_index].depth) +
                      1;
        if (bone.depth != expected_depth ||
            std::ranges::any_of(
                bone.pivot,
                [](float value) { return !std::isfinite(value); })) {
            return Corrupt();
        }
    }

    std::vector<uint32_t> open_bones;
    open_bones.reserve(bone_count);
    for (uint32_t index = 0; index < bone_count; ++index) {
        const auto parent = bones.list[index].parent_index;
        while (!open_bones.empty() && open_bones.back() != parent) {
            if (bones.list[open_bones.back()].subtree_end != index) {
                return Corrupt();
            }
            open_bones.pop_back();
        }
        if (parent != UINT32_MAX &&
            (open_bones.empty() || open_bones.back() != parent)) {
            return Corrupt();
        }
        open_bones.push_back(index);
    }
    while (!open_bones.empty()) {
        if (bones.list[open_bones.back()].subtree_end != bone_count) {
            return Corrupt();
        }
        open_bones.pop_back();
    }
    return OkStatus();
}

template <simd::Width kWidth>
absl::Status ValidateModel(BakedModel& model) {
    auto& bones = BakedModelSerdeAccess::Bones(model);
    auto& cubes = BakedModelSerdeAccess::Cubes<kWidth>(model);
    YSM_RETURN_IF_ERROR(ValidateHierarchy(bones));
    YSM_RETURN_IF_ERROR(ValidateGroups(cubes.cutout, bones.list.size()));
    YSM_RETURN_IF_ERROR(
        ValidateGroups(cubes.cutout_no_culling, bones.list.size()));
    YSM_RETURN_IF_ERROR(ValidateGroups(cubes.translucent, bones.list.size()));
    YSM_RETURN_IF_ERROR(
        ValidateGroups(cubes.translucent_culling, bones.list.size()));

    auto group_info_cache = std::span(bones.cube_group_info_cache);
    size_t cache_offset = 0;
    for (size_t index = 0; index < bones.list.size(); ++index) {
        auto& bone = bones.list[index];
        YSM_RETURN_IF_ERROR(ValidatePartition(bone.cutout, cubes.cutout, index,
                                              group_info_cache, cache_offset));
        YSM_RETURN_IF_ERROR(ValidatePartition(
            bone.cutout_no_culling, cubes.cutout_no_culling, index,
            group_info_cache, cache_offset));
        YSM_RETURN_IF_ERROR(ValidatePartition(
            bone.translucent, cubes.translucent, index, group_info_cache,
            cache_offset));
        YSM_RETURN_IF_ERROR(ValidatePartition(
            bone.translucent_culling, cubes.translucent_culling, index,
            group_info_cache, cache_offset));
    }
    if (cache_offset != group_info_cache.size()) {
        return Corrupt();
    }
    return OkStatus();
}

template <simd::Width kWidth>
absl::StatusOr<std::unique_ptr<BakedModel>> ReadForWidth(const char* data,
                                                         size_t size) {
    BakedModel model(
        BakedModelInfo{}, BakedModelBones{}, BakedModelCubes<kWidth>{});
    serde::BakedModelBody<kWidth> body{model};
    size_t consumed = 0;
    if (struct_pack::deserialize_to(body, data, size, consumed) ||
        consumed != size) {
        return Corrupt();
    }
    YSM_RETURN_IF_ERROR(ValidateModel<kWidth>(model));
    return std::make_unique<BakedModel>(std::move(model));
}
}  // namespace

size_t BakedModelSerializationBytes(const BakedModel& model) {
    return model.VisitCubes([&]<simd::Width kWidth>(
                                const BakedModelCubes<kWidth>&) {
        const auto header = struct_pack::get_needed_size(
            serde::BakedModelHeader{static_cast<uint8_t>(simd::bytes(kWidth))});
        const auto body = struct_pack::get_needed_size(
            serde::BakedModelBody<kWidth>{const_cast<BakedModel&>(model)});
        return header.size() + body.size();
    });
}

BufferManaged SerializeBakedModel(const BakedModel& model) {
    BufferManaged output;
    output.reserve(BakedModelSerializationBytes(model));
    model.VisitCubes(
        [&]<simd::Width kWidth>(const BakedModelCubes<kWidth>&) {
            const auto header = struct_pack::serialize(
                serde::BakedModelHeader{static_cast<uint8_t>(
                    simd::bytes(kWidth))});
            struct_pack::serialize_to_with_offset(
                output, header.size(), serde::BakedModelBody<kWidth>{
                    const_cast<BakedModel&>(model)});
            std::memcpy(output.data(), header.data(), header.size());
        });
    return output;
}

absl::StatusOr<std::unique_ptr<BakedModel>> ReadBakedModel(
    BufferViewR baked_model_data) {
    serde::BakedModelHeader header;
    size_t header_size = 0;
    const auto* data = reinterpret_cast<const char*>(baked_model_data.data());
    if (struct_pack::deserialize_to(header, data, baked_model_data.size(),
                                    header_size) ||
        header_size >= baked_model_data.size()) {
        return Corrupt();
    }

    const auto expected_vec_bytes = magic_enum::enum_switch(
        [](auto kType) {
            return static_cast<uint8_t>(simd::bytes(simd::width<kType>()));
        },
        simd::kSupported);
    if (header.vec_bytes != expected_vec_bytes) {
        return Corrupt();
    }

    data += header_size;
    const auto size = baked_model_data.size() - header_size;
    switch (header.vec_bytes) {
        case simd::bytes(simd::Width::B128):
            return ReadForWidth<simd::Width::B128>(data, size);
        case simd::bytes(simd::Width::B256):
            return ReadForWidth<simd::Width::B256>(data, size);
        case simd::bytes(simd::Width::B512):
            return ReadForWidth<simd::Width::B512>(data, size);
        default:
            return Corrupt();
    }
}
}  // namespace ysm::gfx::bake
