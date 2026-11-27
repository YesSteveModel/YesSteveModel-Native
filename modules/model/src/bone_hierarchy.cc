#include "model/bone_hierarchy.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>

#include <absl/container/flat_hash_map.h>

#include "proto/asset/model/data/geo_model.proto.h"

namespace ysm::model {
namespace {
bool IsFinite(std::span<const float> values) noexcept {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}
}  // namespace

absl::StatusOr<BoneHierarchy> BuildBoneHierarchy(
    const proto::asset::model::data::GeoModel& model) {
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
        std::copy_n(model.bones[original].rotate.begin(), 3, info.bind_rotation.begin());
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
}  // namespace ysm::model
