#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include <absl/status/statusor.h>

#include "gfx/math/pose_stack.h"
#include "gfx/bake/baked_model.h"
#include "bone_attribute.h"
#include "color.h"
#include "schedule.h"

namespace ysm::gfx::renderer {
struct alignas(64) BonePose {
    mat4 pose = GLM_MAT4_IDENTITY_INIT;
    mat3 normal = GLM_MAT3_IDENTITY_INIT;

    bool uniform_scale = true;
    float tangent_orientation = 1.0f;
    float normal_scale = 1.0f;

    Color color{.packed = 0xFFFFFFFF};
    uint8_t glowing = 0xFFu;
};

static_assert(std::is_standard_layout_v<BonePose>);
static_assert(std::is_trivially_copyable_v<BonePose>);
static_assert(alignof(BonePose) == 64);
static_assert(sizeof(bool) == 1);
static_assert(offsetof(BonePose, normal) == 64);
static_assert(offsetof(BonePose, uniform_scale) == 100);
static_assert(offsetof(BonePose, tangent_orientation) == 104);
static_assert(offsetof(BonePose, normal_scale) == 108);
static_assert(offsetof(BonePose, color) == 112);
static_assert(offsetof(BonePose, glowing) == 116);
static_assert(sizeof(BonePose) == 128);

struct ModelPoseView {
    std::span<const BonePose> bone_poses;
    std::span<const uint16_t> render_bone_indices;
};

class ModelState {
   public:
    struct ExtractOutput {
        uint32_t vertex_count = 0;
        uint32_t locator_count = 0;
        bool schedule_updated = false;
    };

    template <typename SimdTag>
    absl::StatusOr<ExtractOutput> Extract(
        SimdTag tag,
        const std::shared_ptr<bake::BakedModel>& baked_model,
        std::span<const BoneAttribute> bone_attributes,
        size_t locator_capacity);

    template <typename SimdTag>
    absl::StatusOr<ExtractOutput> ExtractSubtree(
        SimdTag tag,
        const std::shared_ptr<bake::BakedModel>& baked_model,
        std::span<const BoneAttribute> bone_attributes,
        size_t locator_capacity, size_t root_index);

    void Invalidate() noexcept { valid_ = false; }

    [[nodiscard]] std::span<const uint16_t> StagedLocatorBoneIndices(
        size_t count) const {
        return std::span(locator_bone_indices_scratch_).first(count);
    }

    [[nodiscard]] bool IsValid() const noexcept { return valid_; }

    [[nodiscard]] const std::shared_ptr<bake::BakedModel>& Model() const {
        return baked_model_;
    }

    [[nodiscard]] size_t RenderBoneSize() const noexcept {
        return render_bone_indices_.size();
    }

    [[nodiscard]] const RenderSchedule& Schedule() const { return schedule_; }

    [[nodiscard]] ModelPoseView PoseView() const {
        return {bone_poses_, render_bone_indices_};
    }

   private:
    template <typename SimdTag>
    struct Extraction;

    std::shared_ptr<bake::BakedModel> baked_model_;
    std::vector<BonePose> bone_poses_;
    std::vector<uint16_t> render_bone_indices_;
    std::vector<uint16_t> locator_bone_indices_scratch_;
    RenderSchedule schedule_;
    size_t schedule_worker_count_ = 0;
    bool schedule_valid_ = false;
    bool valid_ = false;
};
}  // namespace ysm::gfx::renderer
