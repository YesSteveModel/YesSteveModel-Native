#include "gfx/renderer/render.h"

#include <utility>

#define YSM_TRANSFORM_USE_APPROXIMATE_RSQRT 1

#include "cpu.h"
#include "enum.h"
#include "profile.h"

#include "gfx/renderer/model_state.h"
#include "gfx/renderer/schedule.h"
#include "buffer/translucent.h"
#include "cube/output.h"
#include "cube/transform.h"
#include "parallel_executor.h"
#include "render_state.h"
#include "vertex_trait.h"

#define YSM_INSTANTIATE_FILE "renderer/render_impl.inc"
#include "instantiate_targets.inc"
#undef YSM_INSTANTIATE_FILE

namespace ysm::gfx::renderer {
namespace {
struct WorkerReadyWaiter {
    std::span<const uint8_t> bone_owners;
    std::span<const WorkerReadyFlag> ready_flags;
    uint64_t ready_mask = 0;

    WorkerReadyWaiter(const RenderSchedule& schedule,
                      const RenderState& render_state)
        : bone_owners(schedule.bone_update_owners),
          ready_flags(render_state.WorkerReadyFlags()) {
    }

    [[nodiscard]] bool Wait(uint32_t bone_index) noexcept {
        if (bone_index >= bone_owners.size()) [[unlikely]] {
            return false;
        }
        const auto owner = bone_owners[bone_index];
        if (owner >= ready_flags.size() || owner >= 64) [[unlikely]] {
            return false;
        }
        const auto owner_mask = uint64_t{1} << owner;
        if ((ready_mask & owner_mask) == 0) {
            const auto& ready = ready_flags[owner].value;
            while (!ready.load(std::memory_order_acquire)) {
                ysm_pause;
            }
            ready_mask |= owner_mask;
        }
        return true;
    }
};

struct NopWaiter {
    NopWaiter(auto&&...) {}
    [[nodiscard]] bool Wait(auto&&) const noexcept { return true; }
};

template <RenderContext kContext>
consteval std::string_view ProfileRenderContext() noexcept {
    if constexpr (kContext == RenderContext::kLevel) {
        return "YSM/C++/Render/LEVEL"sv;
    } else if constexpr (kContext == RenderContext::kIrisShadow) {
        return "YSM/C++/Render/IRIS_SHADOW"sv;
    } else {
        static_assert(kContext == RenderContext::kGui);
        return "YSM/C++/Render/GUI"sv;
    }
}
}

absl::Status Render(BufferView vertex_buffer, VertexKind vertex_kind,
                    const ModelState& model_state,
                    const RenderParameters& parameters, RenderResult& result) {
    result = RenderResult::kStateInvalid;
    YSM_ASSERT(model_state.IsValid(),
               absl::FailedPreconditionError("ModelState is invalid."));
    const auto& schedule = model_state.Schedule();
    const auto& baked_model = *model_state.Model();
    const auto has_pbr = baked_model.Info().has_pbr;
    const auto schedule_mode = schedule.mode;
    auto& executor = ParallelExecutor::Get();

    thread_local RenderState local_render_state;
    auto& state = local_render_state;

    return EnumSwitch([&](auto kVertexKind, auto kSimdType, auto kRenderContext, auto kHasPbr, auto kWorkerReadySpin) {
        using VertexType =
            VertexType<kVertexKind, kSimdType, kRenderContext>;
        using VertexBufferType =
            VertexBufferType<kVertexKind, kSimdType, kRenderContext>;
        static constexpr simd::Width kWidth = simd::width<kSimdType>();
        using TranslucentBufferType =
            buffer::TranslucentProxyBuffer<VertexBufferType>;
        static constexpr simd::Tag<kSimdType> tag;

        YSM_PROFILE_ZONE(ProfileRenderContext<kRenderContext>().data());
        YSM_PROFILE_VALUE(model_state.Schedule().vertex_count);

        result = RenderResult::kOutputRejected;
        YSM_ASSERT(vertex_buffer.size() >=
            VertexBufferType::kVertexSize * schedule.vertex_count,
            absl::InvalidArgumentError("Vertex buffer too small"));
        YSM_RETURN_IF_ERROR(state.UpdateCommon(tag, parameters, model_state));
        result = RenderResult::kStateInvalid;

        [[maybe_unused]] auto active_scope = ParallelExecutor::NopScope();
        if (schedule_mode == RenderSchedulingMode::kSerialPrewake) {
            active_scope = executor.BeginActiveScope();
        }
        if (schedule_mode != RenderSchedulingMode::kWorkerReadySpin) {
            result = state.Update(tag, parameters, model_state, 0, 1);
            YSM_ASSERT(result == RenderResult::kSuccess,
                       absl::InvalidArgumentError("Error updating render bone state"));
        }
        if (active_scope == nullptr && schedule_mode != RenderSchedulingMode::kInline) {
            active_scope = executor.BeginActiveScope();
        }

        const auto& cubes = baked_model.Cubes<kWidth>();
        VertexBufferType vertex_consumer(
            {reinterpret_cast<VertexType*>(vertex_buffer.data()),
             schedule.vertex_count},
            parameters);
        TranslucentBufferType translucent_proxy(
            vertex_consumer, schedule.translucent_vertex_count,
            schedule.translucent_vertex_offset);
        std::atomic<RenderResult> generation_result = RenderResult::kSuccess;
        auto record_result = [&](RenderResult current) {
            if (current == RenderResult::kStateInvalid) [[unlikely]] {
                generation_result.store(current, std::memory_order_relaxed);
            } else if (current == RenderResult::kOutputRejected) [[unlikely]] {
                auto expected = RenderResult::kSuccess;
                generation_result.compare_exchange_strong(
                    expected, current, std::memory_order_relaxed);
            }
        };

        auto task_func = [&](uint32_t worker_index, uint32_t worker_count) {
            std::conditional_t<kWorkerReadySpin, WorkerReadyWaiter, NopWaiter>
                bone_waiter(schedule, state);

            if constexpr (kWorkerReadySpin) {
                auto current = state.Update(tag, parameters, model_state, worker_index,
                                           worker_count);
                state.PublishWorkerReady(worker_index);
                if (current != RenderResult::kSuccess) [[unlikely]] {
                    record_result(current);
                    return;
                }
            }

            auto& task = schedule.tasks[worker_index];
            // cutout
            if (auto& partition = task.cutout;
                !partition.cube_indices.empty()) {
                YSM_PROFILE_ZONE("YSM/C++/Partition/Cutout");
                YSM_PROFILE_VALUE(partition.expected_vertex_count);
                record_result(PerformRender<true, kHasPbr>(
                    tag, vertex_consumer, cubes.cutout, state, partition,
                    parameters, bone_waiter));
            }
            if (auto& partition = task.cutout_no_culling;
                !partition.cube_indices.empty()) {
                YSM_PROFILE_ZONE("YSM/C++/Partition/CutoutNoCulling");
                YSM_PROFILE_VALUE(partition.expected_vertex_count);
                record_result(PerformRender<false, kHasPbr>(
                    tag, vertex_consumer, cubes.cutout_no_culling, state,
                    partition, parameters, bone_waiter));
            }
            // translucent
            if (auto& partition = task.translucent;
                !partition.cube_indices.empty()) {
                YSM_PROFILE_ZONE("YSM/C++/Partition/Translucent");
                YSM_PROFILE_VALUE(partition.expected_vertex_count);
                record_result(PerformRender<false, kHasPbr>(
                    tag, translucent_proxy, cubes.translucent, state,
                    partition, parameters, bone_waiter));
            }
            if (auto& partition = task.translucent_culling;
                !partition.cube_indices.empty()) {
                YSM_PROFILE_ZONE("YSM/C++/Partition/TranslucentCulling");
                YSM_PROFILE_VALUE(partition.expected_vertex_count);
                record_result(PerformRender<true, kHasPbr>(
                    tag, translucent_proxy, cubes.translucent_culling, state,
                    partition, parameters, bone_waiter));
            }
        };

        if (schedule_mode == RenderSchedulingMode::kInline) {
            task_func(0, 1);
        } else {
            executor.Execute(task_func);
            ysm_lfence;
        }

        result = generation_result.load(std::memory_order_relaxed);
        if (result != RenderResult::kSuccess) [[unlikely]] {
            return absl::InternalError("Error updating render bone state");
        }
        {
            YSM_PROFILE_ZONE("YSM/C++/Translucent.Flush");
            YSM_PROFILE_VALUE(schedule.translucent_vertex_count);
            translucent_proxy.Flush(parameters.ctx !=
                                    RenderContext::kIrisShadow);
        }

        result = RenderResult::kSuccess;
        return OkStatus();
    }, vertex_kind, simd::kSupported, parameters.ctx, has_pbr,
       schedule_mode == RenderSchedulingMode::kWorkerReadySpin);
}
}  // namespace ysm::gfx::renderer
