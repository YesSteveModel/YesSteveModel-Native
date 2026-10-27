#pragma once

#include <any>
#include <array>
#include <mdspan>
#include <memory>
#include <span>
#include <vector>

#include <cglm/vec2.h>
#include <magic_enum_switch.hpp>

#include "buffer_scalar.h"
#include "cpu.h"
#include "empty.h"
#include "non_copyable.h"

namespace ysm::gfx::bake {
struct BakedModelSerdeAccess;

struct CubeAttribute {
    struct QuadAttr {
        std::array<vec2, 4> uv{};
        vec2 mid_uv{};
    };

    std::array<QuadAttr, 6> quad_attr{};
    uint8_t quad_count = 0;
    uint8_t quad_count_after_culling = 0;
};

template <simd::Width kVecWidthIn, bool kTranslucentIn>
struct alignas(simd::bytes(kVecWidthIn)) CubeGroup {
    using CubeAttr = CubeAttribute;

    static constexpr simd::Width kVecWidth = kVecWidthIn;
    static constexpr bool kTranslucent = kTranslucentIn;

    static constexpr size_t kCubeGroupCapacity =
        (kVecWidth == simd::Width::B256) ? 1 : 2;
    static constexpr size_t kQuadAttrCapacity =
        kVecWidth == simd::Width::B128 ? 12 : (kCubeGroupCapacity * 8);

    std::array<std::array<float, 8 * kCubeGroupCapacity>, 3> pos{};
    YSM_EMPTY_OR(kTranslucent,
                 std::array<std::array<float, kQuadAttrCapacity>, 3>) center;
    std::array<std::array<float, kQuadAttrCapacity>, 3> normal{};
    std::array<float, kQuadAttrCapacity> plane_d{};
    std::array<float, kQuadAttrCapacity> winding_sign{};
    std::array<std::array<float, kQuadAttrCapacity>, 4> tangent{};
    std::array<std::array<uint32_t, kQuadAttrCapacity>, 4> vertex_index{};
    std::array<CubeAttr, kCubeGroupCapacity> cube_attr{};

    uint32_t cube_count = 0;
    uint32_t bone_index = 0;

    static uint32_t GetQuadAttrIndex(uint32_t cubeIndex,
                                     uint32_t quadIndex) noexcept {
        if constexpr (kVecWidth == simd::Width::B128) {
            return quadIndex + cubeIndex * 6;
        } else if constexpr (kCubeGroupCapacity > 1) {
            return quadIndex + cubeIndex * 8;
        } else {
            return quadIndex;
        }
    }

    [[nodiscard]] bool IsSingleQuad() const noexcept {
        return cube_count == 1 && cube_attr[0].quad_count == 1;
    }

    void NormalizeSingleQuadVertexLayout() noexcept {
        if (!IsSingleQuad()) {
            return;
        }

        const uint32_t attr_index = GetQuadAttrIndex(0, 0);
        std::array<std::array<float, 4>, 3> ordered_pos{};
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            const auto source_index = vertex_index[vertex][attr_index];
            for (size_t axis = 0; axis < 3; ++axis) {
                ordered_pos[axis][vertex] = pos[axis][source_index];
            }
        }
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            for (size_t axis = 0; axis < 3; ++axis) {
                pos[axis][vertex] = ordered_pos[axis][vertex];
            }
            vertex_index[vertex][attr_index] = static_cast<uint32_t>(vertex);
        }
    }
};

// 渲染用
template <simd::Width kVecWidth>
struct BakedModelCubes {
    std::vector<CubeGroup<kVecWidth, false>> cutout;
    std::vector<CubeGroup<kVecWidth, false>> cutout_no_culling;
    std::vector<CubeGroup<kVecWidth, true>> translucent;
    std::vector<CubeGroup<kVecWidth, true>> translucent_culling;
};

// 调度用
struct BakedModelBones {
    struct CubeGroupInfo {
        uint32_t cube_count = 0;
        uint32_t quad_count = 0;
        uint32_t quad_count_after_culling = 0;
    };

    struct BonePartitionInfo {
        std::span<const uint32_t> cube_indices;
        std::span<const CubeGroupInfo> cube_group_info;
        uint32_t cube_count = 0;
        uint32_t full_vertex_count = 0;
        uint32_t culling_vertex_count = 0;
    };

    struct BoneInfo {
        BonePartitionInfo cutout;
        BonePartitionInfo cutout_no_culling;
        BonePartitionInfo translucent;
        BonePartitionInfo translucent_culling;
        std::array<float, 3> pivot{};
        uint32_t parent_index = UINT32_MAX;
        uint32_t subtree_end = 0;
        uint32_t depth = 0;
        bool solid = false;

        [[nodiscard]] bool HasGeometry() const noexcept {
            return !cutout.cube_indices.empty() ||
                   !cutout_no_culling.cube_indices.empty() ||
                   !translucent.cube_indices.empty() ||
                   !translucent_culling.cube_indices.empty();
        }
    };

    std::vector<BoneInfo> list;
    std::vector<uint16_t> sorted_bone_indices;
    std::vector<uint32_t> cube_indices_cache;
    std::vector<CubeGroupInfo> cube_group_info_cache;
};

struct BakedModelInfo {
    bool gui_no_shadow = false;
    bool has_pbr = false;
};

class BakedModel {
    friend struct BakedModelSerdeAccess;

    BakedModelInfo info_;
    BakedModelBones bones_;
    std::any cubes_ = nullptr;
    simd::Width vec_width_;

   public:
    template <simd::Width kVecWidth>
    explicit BakedModel(BakedModelInfo info, BakedModelBones bones, BakedModelCubes<kVecWidth> cubes)
        : info_(info),
          bones_(std::move(bones)),
          cubes_(std::move(cubes)),
          vec_width_(kVecWidth) {}

    decltype(auto) VisitCubes(auto&& visitor) const {
        return magic_enum::enum_switch(
            [&](auto width) {
                return visitor(
                    std::any_cast<const BakedModelCubes<width>&>(cubes_));
            },
            vec_width_);
    }

    const BakedModelBones& Bones() const { return bones_; }

    const BakedModelInfo& Info() const {
        return info_;
    }

    [[nodiscard]] size_t CapacityBytes() const {
        const auto vector_bytes = [](const auto& values) {
            return values.capacity() * sizeof(typename std::remove_cvref_t<
                                              decltype(values)>::value_type);
        };
        return sizeof(*this) + vector_bytes(bones_.list) +
               vector_bytes(bones_.sorted_bone_indices) +
               vector_bytes(bones_.cube_indices_cache) +
               vector_bytes(bones_.cube_group_info_cache) +
               VisitCubes([&](const auto& cubes) {
                   return sizeof(cubes) + vector_bytes(cubes.cutout) +
                          vector_bytes(cubes.cutout_no_culling) +
                          vector_bytes(cubes.translucent) +
                          vector_bytes(cubes.translucent_culling);
               });
    }

    template <simd::Width kVecWidth>
    const BakedModelCubes<kVecWidth>& Cubes() const {
        if (kVecWidth != vec_width_) [[unlikely]] {
            throw std::invalid_argument("Inconsistent simd width");
        }
        return std::any_cast<const BakedModelCubes<kVecWidth>&>(cubes_);
    }
};

#pragma pack(push, 1)

struct Pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

#pragma pack(pop)
static_assert(sizeof(Pixel) == 4);

using Texture = std::mdspan<
    const Pixel,
    std::extents<std::size_t, std::dynamic_extent, std::dynamic_extent>,
    std::layout_left>;

struct BakeModelOptions {
    uint16_t origin_ver = 0;
    bool force_culling = false;
    bool force_translucent = false;
    bool has_pbr = false;
};

absl::StatusOr<std::unique_ptr<BakedModel>> BakeModel(
    BufferViewR model_data, Texture texture, const BakeModelOptions& options);

absl::Status TryBakeModel(BufferViewR model_data, Texture texture,
                          const BakeModelOptions& options);
}  // namespace ysm::gfx::bake
