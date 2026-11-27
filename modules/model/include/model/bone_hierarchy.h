#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <absl/status/statusor.h>

namespace ysm::proto::asset::model::data {
struct GeoModel;
}

namespace ysm::model {
struct BoneHierarchyInfo {
    std::array<float, 3> pivot{};
    uint32_t parent_index = UINT32_MAX;
    uint32_t subtree_end = 0;
    uint32_t depth = 0;
    std::array<float, 3> bind_rotation{};
};

struct BoneHierarchy {
    std::vector<BoneHierarchyInfo> list;
    std::vector<uint16_t> sorted_bone_indices;
};

// The preorder preserves authored root and sibling order, including in cached baked models.
absl::StatusOr<BoneHierarchy> BuildBoneHierarchy(
    const proto::asset::model::data::GeoModel& model);
}  // namespace ysm::model
