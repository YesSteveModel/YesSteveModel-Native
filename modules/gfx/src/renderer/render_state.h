#pragma once

#include <cglm/mat3.h>
#include <cglm/mat4.h>
#include <cglm/vec4.h>

#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "gfx/bake/baked_model.h"
#include "gfx/renderer/model_state.h"
#include "gfx/renderer/render.h"
#include "system_allocator.h"

namespace ysm::gfx::renderer {
struct alignas(64) WorkerReadyFlag {
    std::atomic_bool value = false;
};

struct alignas(64) RenderBoneState {
    mat4 pose;
    mat3 normal;
    vec4 facing_coeff;
    vec4 depth_z;
    vec4 depth_w;
    float tangent_orientation;
    uint32_t packed_light;
    uint32_t packed_opaque_color;
    uint32_t packed_translucent_color;
    bool uniform_scale;

    RenderResult result = RenderResult::kSuccess;
};

class alignas(64) RenderState {
    mat4 clip_model{};
    vec4 clip_facing{};
    float outer_tangent_orientation = 0;
    bool outer_uniform_scale = false;

    std::vector<RenderBoneState, SystemAllocator<RenderBoneState>> bone_states;
    std::vector<WorkerReadyFlag, SystemAllocator<WorkerReadyFlag>> worker_ready_flags;
    size_t worker_ready_size = 0;

public:
    template <typename SimdTag>
    absl::Status UpdateCommon(SimdTag tag, const RenderParameters& params,
                        const ModelState& model_state);

    template <typename SimdTag>
    RenderResult Update(SimdTag tag, const RenderParameters& params,
                        const ModelState& model_state,
                        uint32_t worker_index, uint32_t worker_count);

    [[nodiscard]] const RenderBoneState& GetBoneState(uint32_t bone_index) const {
        return bone_states.at(bone_index);
    }

    [[nodiscard]] std::span<const WorkerReadyFlag> WorkerReadyFlags() const {
        return {worker_ready_flags.data(), worker_ready_size};
    }

    void PublishWorkerReady(uint32_t worker_index) {
        worker_ready_flags[worker_index].value.store(
            true, std::memory_order_release);
    }
};
}  // namespace ysm::gfx::renderer
