#include "gfx/bake/baked_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <absl/container/flat_hash_map.h>
#include <absl/strings/str_cat.h>
#include <utility>

#include <cglm/vec3.h>

#include "proto/asset/model/data/geo_model.proto.h"

namespace ysm::gfx::bake {
namespace {
namespace model_proto = ysm::proto::asset::model::data;

enum class UvProcessing : uint16_t {
    kLegacy = 0,
    k241 = 6,
    k250 = 17,
    k252 = 28,
    k254 = 29,
};

enum class QuadType {
    kEmpty,
    kTranslucent,
    kCutout,
    kSolid,
};

enum class PartitionKind : size_t {
    kCutout,
    kCutoutNoCulling,
    kTranslucent,
    kTranslucentCulling,
};

struct ProcessedQuad {
    QuadType type = QuadType::kEmpty;
    std::array<uint32_t, 4> pos_indices{};
    std::array<std::array<float, 2>, 4> uv{};
    std::array<float, 3> normal{};
    float plane_d = 0.0f;
    float winding_sign = 0.0f;
    std::array<float, 4> tangent{};
    std::array<float, 3> center{};
};

struct ProcessedCube {
    std::array<std::array<float, 3>, 8> pos{};
    std::vector<ProcessedQuad> quads;
    bool inverted = false;
};

bool IsFinite(std::span<const float> values) noexcept;

absl::Status ValidateCube(const model_proto::CubeLegacy& cube,
                          std::string_view location);

struct BoneHierarchyInfo {
    std::array<float, 3> pivot{};
    uint32_t parent_index = UINT32_MAX;
    uint32_t subtree_end = 0;
    uint32_t depth = 0;
};

struct BoneHierarchy {
    std::vector<BoneHierarchyInfo> list;
    std::vector<uint16_t> sorted_bone_indices;
};

struct PreparedBakeModel {
    model_proto::GeoModel model;
    std::vector<size_t> cube_offsets;
    BoneHierarchy hierarchy;
};

absl::StatusOr<BoneHierarchy> BuildBoneHierarchy(
    const model_proto::GeoModel& model) {
    const auto bone_count = model.bones.size();
    if (bone_count > static_cast<size_t>(UINT16_MAX) + 1ULL) {
        return absl::InvalidArgumentError("Too many bones.");
    }

    absl::flat_hash_map<std::string_view, uint32_t> original_by_name;
    original_by_name.reserve(bone_count);
    for (uint32_t i = 0; i < bone_count; ++i) {
        const auto& bone = model.bones[i];
        if (!original_by_name.emplace(bone.name, i).second ||
            bone.pivot.size() != 3 || bone.rotate.size() != 3 ||
            !IsFinite(bone.pivot) || !IsFinite(bone.rotate)) {
            return absl::InvalidArgumentError("Invalid bone metadata.");
        }
    }

    std::vector<std::vector<uint32_t>> children(bone_count);
    std::vector<uint32_t> roots;
    roots.reserve(bone_count);
    for (uint32_t original = 0; original < bone_count; ++original) {
        const auto& parent_name = model.bones[original].parent;
        if (!parent_name.has_value() || parent_name->empty()) {
            roots.push_back(original);
            continue;
        }
        const auto parent = original_by_name.find(*parent_name);
        if (parent == original_by_name.end()) {
            return absl::InvalidArgumentError("Bone parent not found.");
        }
        children[parent->second].push_back(original);
    }

    struct TraversalFrame {
        uint32_t original_index;
        uint32_t sorted_index;
        size_t next_child = 0;
    };

    BoneHierarchy output;
    output.list.reserve(bone_count);
    output.sorted_bone_indices.reserve(bone_count);
    std::vector<uint8_t> visit_state(bone_count);
    std::vector<TraversalFrame> stack;
    stack.reserve(bone_count);

    const auto append_bone = [&](uint32_t original, uint32_t parent) {
        const auto sorted = static_cast<uint32_t>(output.list.size());
        output.sorted_bone_indices.push_back(static_cast<uint16_t>(original));
        auto& info = output.list.emplace_back();
        std::copy_n(model.bones[original].pivot.begin(), 3, info.pivot.begin());
        info.parent_index = parent;
        info.depth = parent == UINT32_MAX ? 0 : output.list[parent].depth + 1;
        visit_state[original] = 1;
        stack.push_back({original, sorted});
    };

    for (const auto root : roots) {
        if (visit_state[root] != 0) {
            return absl::InvalidArgumentError("Invalid bone hierarchy.");
        }
        append_bone(root, UINT32_MAX);
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto& bone_children = children[frame.original_index];
            if (frame.next_child < bone_children.size()) {
                const auto child = bone_children[frame.next_child++];
                if (visit_state[child] != 0) {
                    return absl::InvalidArgumentError(
                        "Invalid bone hierarchy.");
                }
                append_bone(child, frame.sorted_index);
                continue;
            }
            output.list[frame.sorted_index].subtree_end =
                static_cast<uint32_t>(output.list.size());
            visit_state[frame.original_index] = 2;
            stack.pop_back();
        }
    }

    if (output.list.size() != bone_count) {
        return absl::InvalidArgumentError("Invalid bone hierarchy.");
    }
    return output;
}

absl::Status ValidateTexture(Texture texture) {
    if (texture.data_handle() == nullptr || texture.extent(0) == 0 ||
        texture.extent(1) == 0 ||
        texture.extent(0) > std::numeric_limits<uint32_t>::max() ||
        texture.extent(1) > std::numeric_limits<uint32_t>::max()) {
        return absl::InvalidArgumentError("Invalid model texture.");
    }
    return absl::OkStatus();
}

absl::StatusOr<PreparedBakeModel> PrepareBakeModel(BufferViewR model_data) {
    PreparedBakeModel prepared;
    try {
        prepared.model.from_pb(BufStr(model_data));
    } catch (const std::exception& exception) {
        return absl::InvalidArgumentError(
            std::string("Invalid geometry protobuf: ") + exception.what());
    }
    prepared.cube_offsets.resize(prepared.model.bones.size() + 1);
    for (size_t i = 0; i < prepared.model.bones.size(); ++i) {
        const auto cube_count = prepared.model.bones[i].cubeCount.value_or(0);
        if (cube_count >
            prepared.model.cubes.cubes_legacy.size() -
                prepared.cube_offsets[i]) {
            return absl::InvalidArgumentError("Invalid bone cube count.");
        }
        prepared.cube_offsets[i + 1] =
            prepared.cube_offsets[i] + cube_count;
    }
    if (prepared.cube_offsets.back() !=
        prepared.model.cubes.cubes_legacy.size()) {
        return absl::InvalidArgumentError("Invalid bone cube count.");
    }
    for (size_t bone_index = 0; bone_index < prepared.model.bones.size();
         ++bone_index) {
        const auto first = prepared.cube_offsets[bone_index];
        const auto count =
            prepared.model.bones[bone_index].cubeCount.value_or(0);
        for (size_t cube_index = 0; cube_index < count; ++cube_index) {
            YSM_RETURN_IF_ERROR(ValidateCube(
                prepared.model.cubes.cubes_legacy[first + cube_index],
                absl::StrCat("bone[", bone_index, "] \"",
                             prepared.model.bones[bone_index].name, "\" cube[",
                             cube_index, "]")));
        }
    }
    YSM_ASSIGN_OR_RETURN(prepared.hierarchy,
                         BuildBoneHierarchy(prepared.model));
    return prepared;
}

void ComputeTangent(ProcessedQuad& quad,
                    const std::array<std::array<float, 3>, 8>& pos) noexcept {
    std::array<float, 3> edge1{};
    std::array<float, 3> edge2{};
    const auto& pos0 = pos[quad.pos_indices[0]];
    const auto& pos1 = pos[quad.pos_indices[1]];
    const auto& pos2 = pos[quad.pos_indices[2]];
    for (size_t axis = 0; axis < 3; ++axis) {
        edge1[axis] = pos1[axis] - pos0[axis];
        edge2[axis] = pos2[axis] - pos0[axis];
    }

    const float delta_u1 = quad.uv[1][0] - quad.uv[0][0];
    const float delta_v1 = quad.uv[1][1] - quad.uv[0][1];
    const float delta_u2 = quad.uv[2][0] - quad.uv[0][0];
    const float delta_v2 = quad.uv[2][1] - quad.uv[0][1];
    const float denominator = delta_u1 * delta_v2 - delta_u2 * delta_v1;
    const float factor = denominator != 0.0f ? 1.0f / denominator : 1.0f;

    std::array<float, 3> bitangent{};
    float tangent_length_sq = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        quad.tangent[axis] =
            (edge1[axis] * delta_v2 - edge2[axis] * delta_v1) * factor;
        tangent_length_sq += quad.tangent[axis] * quad.tangent[axis];
        bitangent[axis] =
            (edge2[axis] * delta_u1 - edge1[axis] * delta_u2) * factor;
    }
    if (tangent_length_sq != 0.0f) {
        const float inverse_length = 1.0f / std::sqrt(tangent_length_sq);
        for (size_t axis = 0; axis < 3; ++axis) {
            quad.tangent[axis] *= inverse_length;
        }
    }

    const std::array<float, 3> tangent_cross_normal{
        quad.tangent[1] * quad.normal[2] - quad.tangent[2] * quad.normal[1],
        quad.tangent[2] * quad.normal[0] - quad.tangent[0] * quad.normal[2],
        quad.tangent[0] * quad.normal[1] - quad.tangent[1] * quad.normal[0],
    };
    float handedness = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        handedness += bitangent[axis] * tangent_cross_normal[axis];
    }
    quad.tangent[3] =
        handedness < 0.0f ? -1.0f : (handedness > 0.0f ? 1.0f : 0.0f);
}

void ComputeFacePlane(ProcessedQuad& quad,
                      const std::array<std::array<float, 3>, 8>& pos) noexcept {
    const auto& pos0 = pos[quad.pos_indices[0]];
    const auto& pos1 = pos[quad.pos_indices[1]];
    const auto& pos2 = pos[quad.pos_indices[2]];
    vec3 edge1{
        pos1[0] - pos0[0],
        pos1[1] - pos0[1],
        pos1[2] - pos0[2],
    };
    vec3 edge2{
        pos2[0] - pos0[0],
        pos2[1] - pos0[1],
        pos2[2] - pos0[2],
    };
    vec3 geometric_normal;
    glm_vec3_cross(edge1, edge2, geometric_normal);

    quad.plane_d = -(quad.normal[0] * pos0[0] + quad.normal[1] * pos0[1] +
                     quad.normal[2] * pos0[2]);
    const auto orientation = quad.normal[0] * geometric_normal[0] +
                             quad.normal[1] * geometric_normal[1] +
                             quad.normal[2] * geometric_normal[2];
    quad.winding_sign =
        orientation < 0.0f ? -1.0f : (orientation > 0.0f ? 1.0f : 0.0f);
}

UvProcessing SelectUvProcessing(uint16_t origin_ver) noexcept {
    if (origin_ver >= static_cast<uint16_t>(UvProcessing::k254)) {
        return UvProcessing::k254;
    }
    if (origin_ver >= static_cast<uint16_t>(UvProcessing::k252)) {
        return UvProcessing::k252;
    }
    if (origin_ver >= static_cast<uint16_t>(UvProcessing::k250)) {
        return UvProcessing::k250;
    }
    if (origin_ver >= static_cast<uint16_t>(UvProcessing::k241)) {
        return UvProcessing::k241;
    }
    return UvProcessing::kLegacy;
}

bool IsFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(
        values, [](float value) { return std::isfinite(value); });
}

absl::Status ValidateCube(const model_proto::CubeLegacy& cube,
                          std::string_view location) {
    const auto invalid = [&](std::string_view condition) {
        return absl::InvalidArgumentError(
            absl::StrCat("Invalid cube geometry at ", location, ": ",
                         condition, "."));
    };
    if (cube.faceCount > 6) [[unlikely]] {
        return invalid(
            absl::StrCat("face_count exceeds 6 (got ", cube.faceCount, ")"));
    }
    if (cube.pos.size() % 3 != 0) [[unlikely]] {
        return invalid(absl::StrCat("position component count is not divisible "
                                    "by 3 (got ",
                                    cube.pos.size(), ")"));
    }
    if (cube.pos.size() / 3 > 8) [[unlikely]] {
        return invalid(absl::StrCat("position count exceeds 8 (got ",
                                    cube.pos.size() / 3, ")"));
    }
    if (cube.pos_indices.size() != cube.faceCount * 4ULL) [[unlikely]] {
        return invalid(absl::StrCat(
            "position index count does not equal face_count * 4 (face_count=",
            cube.faceCount, ", got ", cube.pos_indices.size(), ")"));
    }
    if (cube.uv.size() % 2 != 0) [[unlikely]] {
        return invalid(absl::StrCat(
            "UV component count is not divisible by 2 (got ", cube.uv.size(),
            ")"));
    }
    if (cube.uv_indices.size() != cube.faceCount * 4ULL) [[unlikely]] {
        return invalid(absl::StrCat(
            "UV index count does not equal face_count * 4 (face_count=",
            cube.faceCount, ", got ", cube.uv_indices.size(), ")"));
    }
    if (cube.normal.size() != cube.faceCount * 3ULL) [[unlikely]] {
        return invalid(absl::StrCat(
            "normal component count does not equal face_count * 3 "
            "(face_count=",
            cube.faceCount, ", got ", cube.normal.size(), ")"));
    }
    if (!IsFinite(cube.pos)) [[unlikely]] {
        return invalid("position contains a non-finite value");
    }
    if (!IsFinite(cube.uv)) [[unlikely]] {
        return invalid("UV contains a non-finite value");
    }
    if (!IsFinite(cube.normal)) [[unlikely]] {
        return invalid("normal contains a non-finite value");
    }
    const auto pos_count = cube.pos.size() / 3;
    const auto uv_count = cube.uv.size() / 2;
    if (std::ranges::any_of(
            cube.pos_indices,
            [=](uint32_t index) { return index >= pos_count; }) ||
        std::ranges::any_of(cube.uv_indices, [=](uint32_t index) {
            return index >= uv_count;
        })) [[likely]] {
        return invalid("position or UV index is out of range");
    }
    return OkStatus();
}

class QuadInspector {
    Texture texture_;
    UvProcessing processing_;

    static uint32_t Quantize(float value, uint32_t extent,
                             float threshold) noexcept {
        const auto scaled = std::clamp(value, 0.0f, 1.0f) * extent;
        auto base = static_cast<uint32_t>(scaled);
        if (base < extent && scaled - static_cast<float>(base) >= threshold) {
            ++base;
        }
        return std::min(base, extent);
    }

   public:
    QuadInspector(Texture texture, uint16_t origin_ver)
        : texture_(texture), processing_(SelectUvProcessing(origin_ver)) {}

    QuadType Process(std::array<std::array<float, 2>, 4>& uv) const noexcept {
        const auto width = static_cast<uint32_t>(texture_.extent(0));
        const auto height = static_cast<uint32_t>(texture_.extent(1));
        std::array<uint32_t, 4> u_pixels{};
        std::array<uint32_t, 4> v_pixels{};

        if (processing_ == UvProcessing::kLegacy ||
            processing_ == UvProcessing::k254) {
            for (size_t i = 0; i < 4; ++i) {
                u_pixels[i] = Quantize(uv[i][0], width, 0.6f);
                v_pixels[i] =
                    Quantize(uv[i][1], height,
                             processing_ == UvProcessing::k254 ? 0.4f : 0.6f);
                uv[i][0] = static_cast<float>(u_pixels[i]) / width;
                uv[i][1] = static_cast<float>(v_pixels[i]) / height;
            }
        } else {
            for (size_t i = 0; i < 4; ++i) {
                u_pixels[i] = static_cast<uint32_t>(
                    std::lround(std::clamp(uv[i][0], 0.0f, 1.0f) * width));
                v_pixels[i] = static_cast<uint32_t>(
                    std::lround(std::clamp(uv[i][1], 0.0f, 1.0f) * height));
            }
        }

        auto [min_u_it, max_u_it] =
            std::minmax_element(u_pixels.begin(), u_pixels.end());
        auto [min_v_it, max_v_it] =
            std::minmax_element(v_pixels.begin(), v_pixels.end());
        uint32_t min_u = *min_u_it;
        uint32_t max_u = *max_u_it;
        uint32_t min_v = *min_v_it;
        uint32_t max_v = *max_v_it;
        float u_offset = 0.0f;
        float v_offset = 0.0f;

        if (processing_ == UvProcessing::k241) {
            if (min_u == max_u && max_u < width) {
                ++max_u;
            }
            if (min_v == max_v && max_v < height) {
                ++max_v;
            }
        } else {
            if (min_u == max_u) {
                if (max_u < width) {
                    ++max_u;
                } else if (min_u > 0) {
                    --min_u;
                    u_offset = -1.0f / width;
                }
            }
            if (min_v == max_v) {
                const bool prefer_grow = processing_ == UvProcessing::kLegacy;
                if (prefer_grow && max_v < height) {
                    ++max_v;
                } else if (min_v > 0) {
                    --min_v;
                    v_offset = -1.0f / height;
                } else if (max_v < height) {
                    ++max_v;
                }
            }
        }

        if (processing_ != UvProcessing::kLegacy &&
            processing_ != UvProcessing::k254) {
            if (processing_ == UvProcessing::k252) {
                uv[0][0] += u_offset;
                uv[3][0] += u_offset;
                uv[2][1] += v_offset;
                uv[3][1] += v_offset;
            } else {
                for (auto& value : uv) {
                    value[0] += u_offset;
                    value[1] += v_offset;
                }
            }
        } else {
            if (u_offset != 0.0f) {
                uv[0][0] += u_offset;
                uv[3][0] += u_offset;
            }
            if (v_offset != 0.0f) {
                uv[2][1] += v_offset;
                uv[3][1] += v_offset;
            }
        }

        min_u = std::min(min_u, width);
        max_u = std::min(max_u, width);
        min_v = std::min(min_v, height);
        max_v = std::min(max_v, height);
        bool empty = true;
        bool has_transparent = false;
        for (uint32_t y = min_v; y < max_v; ++y) {
            for (uint32_t x = min_u; x < max_u; ++x) {
                const auto alpha = texture_[x, y].a;
                if (alpha != 0) {
                    empty = false;
                    if (alpha != 255) {
                        return QuadType::kTranslucent;
                    }
                } else {
                    has_transparent = true;
                }
            }
        }
        if (empty) {
            return QuadType::kEmpty;
        }
        return has_transparent ? QuadType::kCutout : QuadType::kSolid;
    }
};

ProcessedCube ProcessCube(const model_proto::CubeLegacy& cube,
                          const QuadInspector& inspector, bool has_pbr) {
    ProcessedCube output;
    const auto pos_count = cube.pos.size() / 3;
    for (size_t i = 0; i < pos_count; ++i) {
        std::copy_n(cube.pos.data() + i * 3, 3, output.pos[i].begin());
    }
    output.quads.reserve(cube.faceCount);
    std::array<std::array<float, 3>, 6> face_centers{};
    std::array<float, 3> cube_center{};

    for (uint32_t face = 0; face < cube.faceCount; ++face) {
        auto& quad = output.quads.emplace_back();
        std::copy_n(cube.normal.data() + face * 3, 3, quad.normal.begin());
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const auto flat_index = face * 4 + vertex;
            quad.pos_indices[vertex] = cube.pos_indices[flat_index];
            const auto uv_index = cube.uv_indices[flat_index];
            quad.uv[vertex][0] = cube.uv[uv_index * 2];
            quad.uv[vertex][1] = cube.uv[uv_index * 2 + 1];
            const auto& pos = output.pos[quad.pos_indices[vertex]];
            for (size_t axis = 0; axis < 3; ++axis) {
                quad.center[axis] += pos[axis] * 0.25f;
            }
        }
        quad.type = inspector.Process(quad.uv);
        ComputeFacePlane(quad, output.pos);
        if (has_pbr) {
            ComputeTangent(quad, output.pos);
        }
        face_centers[face] = quad.center;
        for (size_t axis = 0; axis < 3; ++axis) {
            cube_center[axis] += quad.center[axis] / cube.faceCount;
        }
    }

    if (cube.faceCount == 6) {
        for (uint32_t face = 0; face < cube.faceCount; ++face) {
            std::array<float, 3> outward{};
            float length_sq = 0.0f;
            float dot = 0.0f;
            for (size_t axis = 0; axis < 3; ++axis) {
                outward[axis] = face_centers[face][axis] - cube_center[axis];
                length_sq += outward[axis] * outward[axis];
                dot += output.quads[face].normal[axis] * outward[axis];
            }
            if (length_sq > 1e-12f && dot < 0.0f) {
                output.inverted = true;
                break;
            }
        }
    }
    return output;
}

template <simd::Width kWidth>
struct BuiltModel {
    BakedModelBones bones;
    BakedModelCubes<kWidth> cubes;
};

template <simd::Width kWidth>
class ModelBaker {
    using CutoutGroup = CubeGroup<kWidth, false>;
    using TranslucentGroup = CubeGroup<kWidth, true>;

    const BakeModelOptions& options_;
    BuiltModel<kWidth> output_;
    std::array<std::vector<std::vector<uint32_t>>, 4> cube_indices_;

    auto& BonePartition(PartitionKind kind, size_t bone_index) {
        auto& bone = output_.bones.list[bone_index];
        switch (kind) {
            case PartitionKind::kCutout:
                return bone.cutout;
            case PartitionKind::kCutoutNoCulling:
                return bone.cutout_no_culling;
            case PartitionKind::kTranslucent:
                return bone.translucent;
            case PartitionKind::kTranslucentCulling:
                return bone.translucent_culling;
        }
        std::unreachable();
    }

    template <bool kTranslucent>
    auto& Groups(PartitionKind kind) {
        if constexpr (kTranslucent) {
            return kind == PartitionKind::kTranslucent
                       ? output_.cubes.translucent
                       : output_.cubes.translucent_culling;
        } else {
            return kind == PartitionKind::kCutout
                       ? output_.cubes.cutout
                       : output_.cubes.cutout_no_culling;
        }
    }

    template <typename Group>
    static void NormalizeSingleQuadGroups(std::vector<Group>& groups) {
        for (auto& group : groups) {
            group.NormalizeSingleQuadVertexLayout();
        }
    }

    template <bool kTranslucent>
    void AddCube(PartitionKind kind, uint32_t bone_index,
                 const ProcessedCube& source,
                 std::span<const ProcessedQuad* const> quads,
                 uint8_t culling_quad_count) {
        using Group = CubeGroup<kWidth, kTranslucent>;
        auto& groups = Groups<kTranslucent>(kind);
        auto& bone_info = BonePartition(kind, bone_index);
        auto& indices = cube_indices_[static_cast<size_t>(kind)][bone_index];
        Group* group = nullptr;
        if (!groups.empty() && groups.back().bone_index == bone_index &&
            groups.back().cube_count < Group::kCubeGroupCapacity) {
            group = &groups.back();
        } else {
            group = &groups.emplace_back();
            group->bone_index = bone_index;
            indices.push_back(static_cast<uint32_t>(groups.size() - 1));
        }

        const auto cube_index = group->cube_count++;
        auto& cube_attr = group->cube_attr[cube_index];
        cube_attr.quad_count = static_cast<uint8_t>(quads.size());
        cube_attr.quad_count_after_culling = culling_quad_count;
        for (size_t pos_index = 0; pos_index < source.pos.size(); ++pos_index) {
            for (size_t axis = 0; axis < 3; ++axis) {
                group->pos[axis][cube_index * 8 + pos_index] =
                    source.pos[pos_index][axis];
            }
        }
        for (size_t quad_index = 0; quad_index < quads.size(); ++quad_index) {
            const auto& quad = *quads[quad_index];
            const auto attr_index =
                Group::GetQuadAttrIndex(cube_index, quad_index);
            for (size_t axis = 0; axis < 3; ++axis) {
                group->normal[axis][attr_index] = quad.normal[axis];
                if constexpr (kTranslucent) {
                    group->center[axis][attr_index] = quad.center[axis];
                }
            }
            group->plane_d[attr_index] = quad.plane_d;
            group->winding_sign[attr_index] = quad.winding_sign;
            if (options_.has_pbr) {
                for (size_t axis = 0; axis < 4; ++axis) {
                    group->tangent[axis][attr_index] = quad.tangent[axis];
                }
            }
            auto& quad_attr = cube_attr.quad_attr[quad_index];
            for (size_t vertex = 0; vertex < 4; ++vertex) {
                group->vertex_index[vertex][attr_index] =
                    static_cast<uint32_t>(cube_index * 8) +
                    quad.pos_indices[vertex];
                quad_attr.uv[vertex][0] = quad.uv[vertex][0];
                quad_attr.uv[vertex][1] = quad.uv[vertex][1];
                quad_attr.mid_uv[0] += quad.uv[vertex][0] * 0.25f;
                quad_attr.mid_uv[1] += quad.uv[vertex][1] * 0.25f;
            }
        }
        bone_info.full_vertex_count += static_cast<uint32_t>(quads.size() * 4);
        bone_info.culling_vertex_count += culling_quad_count * 4U;
    }

    void AddProcessedCube(uint32_t bone_index, const ProcessedCube& cube) {
        std::array<const ProcessedQuad*, 6> cutout_storage{};
        std::array<const ProcessedQuad*, 6> translucent_storage{};
        size_t cutout_count = 0;
        size_t translucent_count = 0;
        for (const auto& quad : cube.quads) {
            if (options_.force_translucent ||
                quad.type == QuadType::kTranslucent ||
                quad.type == QuadType::kCutout) {
                if (quad.type != QuadType::kEmpty ||
                    options_.force_translucent) {
                    translucent_storage[translucent_count++] = &quad;
                }
            } else if (quad.type == QuadType::kSolid) {
                cutout_storage[cutout_count++] = &quad;
            }
        }
        const auto cutout = std::span(cutout_storage).first(cutout_count);
        const auto translucent =
            std::span(translucent_storage).first(translucent_count);
        const bool full_cube = cube.quads.size() == 6;
        if ((options_.force_culling && full_cube) || cube.inverted) {
            if (!translucent.empty()) {
                const auto cap = cube.inverted
                                     ? std::min<size_t>(5, translucent.size())
                                     : std::min<size_t>(3, translucent.size());
                AddCube<true>(PartitionKind::kTranslucentCulling, bone_index,
                              cube, translucent, static_cast<uint8_t>(cap));
            }
            if (!cutout.empty()) {
                const auto cap = cube.inverted
                                     ? std::min<size_t>(5, cutout.size())
                                     : std::min<size_t>(3, cutout.size());
                AddCube<false>(PartitionKind::kCutout, bone_index, cube, cutout,
                               static_cast<uint8_t>(cap));
            }
            return;
        }
        if (cutout.size() == 6) {
            AddCube<false>(PartitionKind::kCutout, bone_index, cube, cutout, 3);
        } else if (!cutout.empty()) {
            AddCube<false>(
                PartitionKind::kCutoutNoCulling, bone_index, cube, cutout,
                static_cast<uint8_t>(std::min<size_t>(3, cutout.size())));
        }
        if (!translucent.empty()) {
            AddCube<true>(
                PartitionKind::kTranslucent, bone_index, cube, translucent,
                static_cast<uint8_t>(std::min<size_t>(3, translucent.size())));
        }
    }

    template <typename Group>
    void FinishPartition(PartitionKind kind, const std::vector<Group>& groups,
                         size_t& cache_offset) {
        const auto kind_index = static_cast<size_t>(kind);
        auto indices_cache = std::span(output_.bones.cube_indices_cache);
        auto group_info_cache = std::span(output_.bones.cube_group_info_cache);
        for (size_t bone = 0; bone < output_.bones.list.size(); ++bone) {
            const auto& source_indices = cube_indices_[kind_index][bone];
            const auto indices =
                indices_cache.subspan(cache_offset, source_indices.size());
            const auto group_info =
                group_info_cache.subspan(cache_offset, source_indices.size());
            std::copy(source_indices.begin(), source_indices.end(),
                      indices.begin());

            uint32_t cube_count = 0;
            uint32_t quad_count = 0;
            uint32_t quad_count_after_culling = 0;
            for (size_t i = 0; i < indices.size(); ++i) {
                const auto& group = groups[indices[i]];
                cube_count += group.cube_count;
                for (size_t cube = 0; cube < group.cube_count; ++cube) {
                    quad_count += group.cube_attr[cube].quad_count;
                    quad_count_after_culling +=
                        group.cube_attr[cube].quad_count_after_culling;
                }
                group_info[i] = {
                    cube_count,
                    quad_count,
                    quad_count_after_culling,
                };
            }

            auto& destination = BonePartition(kind, bone);
            destination.cube_indices = indices;
            destination.cube_group_info = group_info;
            destination.cube_count = cube_count;
            destination.full_vertex_count = quad_count * 4U;
            destination.culling_vertex_count = quad_count_after_culling * 4U;
            cache_offset += indices.size();
        }
    }

   public:
    ModelBaker(std::span<const BoneHierarchyInfo> hierarchy,
               std::span<const uint16_t> sorted_bone_indices,
               const BakeModelOptions& options)
        : options_(options) {
        output_.bones.sorted_bone_indices.assign(sorted_bone_indices.begin(),
                                                 sorted_bone_indices.end());
        output_.bones.list.resize(hierarchy.size());
        for (size_t i = 0; i < hierarchy.size(); ++i) {
            auto& bone = output_.bones.list[i];
            bone.pivot = hierarchy[i].pivot;
            bone.parent_index = hierarchy[i].parent_index;
            bone.subtree_end = hierarchy[i].subtree_end;
            bone.depth = hierarchy[i].depth;
            bone.solid = true;
        }
        for (auto& partition : cube_indices_) {
            partition.resize(hierarchy.size());
        }
    }

    void AddBone(uint32_t sorted_index,
                 std::span<const model_proto::CubeLegacy> cubes,
                 const QuadInspector& inspector) {
        bool solid = true;
        for (const auto& source : cubes) {
            auto cube = ProcessCube(source, inspector, options_.has_pbr);
            for (const auto& quad : cube.quads) {
                solid &= quad.type == QuadType::kSolid;
            }
            AddProcessedCube(sorted_index, cube);
        }
        output_.bones.list[sorted_index].solid = solid;
    }

    BuiltModel<kWidth> Finish() {
        NormalizeSingleQuadGroups(output_.cubes.cutout);
        NormalizeSingleQuadGroups(output_.cubes.cutout_no_culling);
        NormalizeSingleQuadGroups(output_.cubes.translucent);
        NormalizeSingleQuadGroups(output_.cubes.translucent_culling);

        size_t index_count = 0;
        for (const auto& partition : cube_indices_) {
            for (const auto& indices : partition) {
                index_count += indices.size();
            }
        }
        output_.bones.cube_indices_cache.resize(index_count);
        output_.bones.cube_group_info_cache.resize(index_count);
        size_t offset = 0;
        FinishPartition(PartitionKind::kCutout, output_.cubes.cutout, offset);
        FinishPartition(PartitionKind::kCutoutNoCulling,
                        output_.cubes.cutout_no_culling, offset);
        FinishPartition(PartitionKind::kTranslucent, output_.cubes.translucent,
                        offset);
        FinishPartition(PartitionKind::kTranslucentCulling,
                        output_.cubes.translucent_culling, offset);
        return std::move(output_);
    }
};

template <simd::Width kWidth>
absl::StatusOr<std::unique_ptr<BakedModel>> BakeForWidth(
    const model_proto::GeoModel& model,
    std::span<const uint16_t> sorted_bone_indices,
    Texture texture, const BakeModelOptions& options,
    std::span<const size_t> cube_offsets,
    std::span<const BoneHierarchyInfo> hierarchy) {
    ModelBaker<kWidth> baker(hierarchy, sorted_bone_indices, options);
    QuadInspector inspector(texture, options.origin_ver);
    for (size_t sorted_index = 0; sorted_index < sorted_bone_indices.size();
         ++sorted_index) {
        const auto original_index = sorted_bone_indices[sorted_index];
        const auto first = cube_offsets[original_index];
        const auto count = model.bones[original_index].cubeCount.value_or(0);
        baker.AddBone(static_cast<uint32_t>(sorted_index),
                      std::span(model.cubes.cubes_legacy).subspan(first, count),
                      inspector);
    }
    auto output = baker.Finish();
    return std::make_unique<BakedModel>(
        BakedModelInfo{.has_pbr = options.has_pbr}, std::move(output.bones),
                                         std::move(output.cubes));
}
}  // namespace

absl::StatusOr<std::unique_ptr<BakedModel>> BakeModel(
    BufferViewR model_data, Texture texture, const BakeModelOptions& options) {
    YSM_RETURN_IF_ERROR(ValidateTexture(texture));
    YSM_DECLARE_OR_RETURN(prepared, PrepareBakeModel(model_data));

    return magic_enum::enum_switch(
        [&](auto kType) {
            return BakeForWidth<simd::width<kType>()>(
                prepared.model, prepared.hierarchy.sorted_bone_indices,
                texture, options, prepared.cube_offsets,
                prepared.hierarchy.list);
        },
        simd::kSupported);
}

absl::Status TryBakeModel(BufferViewR model_data, Texture texture,
                          const BakeModelOptions& options) {
    YSM_RETURN_IF_ERROR(ValidateTexture(texture));
    YSM_DECLARE_OR_RETURN(prepared, PrepareBakeModel(model_data));

    absl::Status first_error;
    const auto try_width = [&]<simd::Width kWidth>() {
        auto result = BakeForWidth<kWidth>(
            prepared.model, prepared.hierarchy.sorted_bone_indices, texture,
            options, prepared.cube_offsets, prepared.hierarchy.list);
        if (!result.ok() && first_error.ok()) {
            first_error = result.status();
        }
    };
    try_width.template operator()<simd::Width::B128>();
    try_width.template operator()<simd::Width::B256>();
    try_width.template operator()<simd::Width::B512>();
    return first_error;
}
}  // namespace ysm::gfx::bake
