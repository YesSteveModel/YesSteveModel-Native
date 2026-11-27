#pragma once

#include <array>
#include <cstddef>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstring>

#include <cglm/mat3.h>
#include <cglm/mat4.h>
#include <cglm/affine.h>

#include "system_allocator.h"
#include "fast_math.h"
#include "inline.h"

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::math {
class PoseStack {
   public:
    struct alignas(64) Pose {
        mat4 pose = GLM_MAT4_IDENTITY_INIT;
        mat3 normal = GLM_MAT3_IDENTITY_INIT;
        
        bool uniform_scale = true;
        float tangent_orientation = 1.0f;
        float normal_scale = 1.0f;

        YSM_INLINE void CopyTo(Pose& destination) const noexcept {
            std::memcpy(&destination, this, sizeof(Pose));
        }
    };

    PoseStack() : poses_(12) {}

    YSM_INLINE void Reserve(size_t capacity) {
        poses_.resize(capacity);
    }

    YSM_INLINE void PushPose() {
        const auto next_index = pose_index_ + 1;
        if (next_index == poses_.size()) [[unlikely]] {
            poses_.emplace_back();
        }
        poses_[pose_index_].CopyTo(poses_[next_index]);
        pose_index_ = next_index;
    }

    YSM_INLINE void PopPose(size_t depth) noexcept {
        assert(depth <= pose_index_);
        if (depth > pose_index_) [[unlikely]] {
            return;
        }
        pose_index_ = depth;
    }

    template <typename SimdTag>
    void Translate(SimdTag tag, float x, float y, float z) noexcept;

    template <typename SimdTag>
    void RotateZYX(SimdTag tag, float x, float y, float z) noexcept;

    template <typename SimdTag>
    void RotateQuat(SimdTag tag, const std::array<float, 4>& unit_xyzw) noexcept;

    template <typename SimdTag>
    void Scale(SimdTag tag, float x, float y, float z) noexcept;

    [[nodiscard]] YSM_INLINE const Pose& Last() const noexcept {
        return poses_[pose_index_];
    }

   private:
    std::vector<Pose, SystemAllocator<Pose>> poses_;
    size_t pose_index_ = 0;

    YSM_INLINE static bool IsUniformScale(float x, float y, float z) noexcept {
        return std::abs(x) == std::abs(y) && std::abs(y) == std::abs(z);
    }
};

static_assert(offsetof(PoseStack::Pose, normal) == 64);
static_assert(offsetof(PoseStack::Pose, uniform_scale) == 100);
static_assert(offsetof(PoseStack::Pose, tangent_orientation) == 104);
static_assert(offsetof(PoseStack::Pose, normal_scale) == 108);
static_assert(sizeof(PoseStack::Pose) == 128);
}  // namespace ysm::gfx::renderer

YSM_FAST_MATH_END
