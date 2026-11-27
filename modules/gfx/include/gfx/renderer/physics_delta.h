#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ysm::gfx::renderer {
struct BonePhysicsDelta {
    uint16_t bone_index;
    uint16_t reserved;
    std::array<float, 3> delta_pos;  // Meters in the scaled local pivot frame.
    std::array<float, 4> delta_rot;  // Unit quaternion in (x, y, z, w) order.
};

static_assert(std::is_standard_layout_v<BonePhysicsDelta>);
static_assert(std::is_trivial_v<BonePhysicsDelta>);
static_assert(offsetof(BonePhysicsDelta, delta_pos) == 4);
static_assert(offsetof(BonePhysicsDelta, delta_rot) == 16);
static_assert(sizeof(BonePhysicsDelta) == 32);
}  // namespace ysm::gfx::renderer
