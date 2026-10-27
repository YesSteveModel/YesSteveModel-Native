#ifndef GUARD_RENDER_IMPL
#define GUARD_RENDER_IMPL

#include <span>
#include "assume.h"
#include "fast_math.h"
#include "gfx/renderer/render.h"
#include "cube/output.h"
#include "cube/transform.h"
#include "render_state.h"

#endif

YSM_FAST_MATH_BEGIN
namespace ysm::gfx::renderer {
template <bool kCulling, bool kHasPbr>
YSM_SIMD_TARGET
RenderResult PerformRender(YSM_SIMD_TAG tag, const auto& vertex_consumer, const auto& model,
                   const RenderState& render_state,
                   const RenderTask::Partition& task,
                   const RenderParameters& params,
                   auto& waiter) noexcept {
    using CubeGroupType = std::remove_cvref_t<decltype(model)>::value_type;
    using VertexBufferType = std::remove_cvref_t<decltype(vertex_consumer)>;
    static constexpr bool kTranslucent = VertexBufferType::kTranslucent;
    static constexpr bool kPosOnly = VertexBufferType::kPosOnly;
    static constexpr bool kIris = VertexBufferType::kIris;

    if (task.expected_vertex_count == 0) [[unlikely]] {
        return RenderResult::kSuccess;
    }

    auto vertex_writer = vertex_consumer.CreateVertexWriter(
        task.vertex_offset, task.expected_vertex_count);
    const auto iris_entity_id = params.iris_entity_id;
    const auto overlay = params.overlay;

    for (auto cube_group_index : task.cube_indices) {
        auto& cube_group = model[cube_group_index];
        auto bone_index = cube_group.bone_index;
        if (!waiter.Wait(bone_index)) [[unlikely]] {
            return RenderResult::kStateInvalid;
        }

        auto& bone_state = render_state.GetBoneState(bone_index);
        if (bone_state.result != RenderResult::kSuccess) [[unlikely]] {
            return bone_state.result;
        }

        const auto light = bone_state.packed_light;
        const auto packed_color = kTranslucent
                                      ? bone_state.packed_translucent_color
                                      : bone_state.packed_opaque_color;
        cube::CubeOutput<CubeGroupType> out_cube_group;

        cube::Transform<kCulling, kIris, kHasPbr, kPosOnly>(
            tag, cube_group, bone_state, out_cube_group);

        // 遍历组内 Cube
        const int cube_count = (CubeGroupType::kCubeGroupCapacity > 1)
                                   ? cube_group.cube_count
                                   : 1;  // 期待编译器优化
        YSM_ASSUME(cube_count >= 1);
        YSM_ASSUME(cube_count <= CubeGroupType::kCubeGroupCapacity);
        for (auto cube_index = 0; cube_index < cube_count; cube_index++) {
            const auto& cube_attr = cube_group.cube_attr[cube_index];
            const int quad_count_after_culling =
                cube_attr.quad_count_after_culling;
            const auto quad_count = cube_attr.quad_count;

            // 遍历 Quad、剔除
            int processed_quads = 0;
            YSM_ASSUME(quad_count >= 1);
            YSM_ASSUME(quad_count <= 6);
            for (auto quad_index = 0; quad_index < quad_count; quad_index++) {
                const auto quad_attr_index =
                    CubeGroupType::GetQuadAttrIndex(cube_index, quad_index);
                const auto& quad_attr = cube_attr.quad_attr[quad_index];
                auto is_back_face = out_cube_group.back_face[quad_attr_index];
                if constexpr (kCulling) {
                    if (is_back_face) {
                        continue;
                    }
                    ++processed_quads;
                }
                vertex_writer.BeginQuad();
                if constexpr (kTranslucent) {
                    vertex_writer.SetQuadDepth(
                        out_cube_group.face_depth[quad_attr_index]);
                }

                const auto normal = out_cube_group.normal[quad_attr_index];
                uint32_t tangent = 0;
                if constexpr (kIris && kHasPbr && !kPosOnly) {
                    tangent = out_cube_group.tangent[quad_attr_index];
                }
                //if constexpr (!kGuiNoLighting) {
                // normal = out_cube_group.normal[quad_attr_index];
                //} else {
                //    normal = 0B10000001'00000000;
                //}

                for (uint32_t vertex_order = 0; vertex_order < 4;
                     ++vertex_order) {
                    auto vertex_index =
                        cube_group.vertex_index[vertex_order][quad_attr_index];
                    auto&& out_vertex = vertex_writer.QuadVertex(vertex_order);
                    out_vertex.SetVertex(out_cube_group.pos[0][vertex_index],
                                         out_cube_group.pos[1][vertex_index],
                                         out_cube_group.pos[2][vertex_index],
                                         quad_attr.uv[vertex_order]);
                    out_vertex.SetNormal(normal);
                    if constexpr (!kPosOnly) {
                        out_vertex.SetColor(packed_color);
                        out_vertex.SetOverlay(overlay);
                        out_vertex.SetLight(light);
                        if constexpr (kIris) {
                            out_vertex.SetIrisEntityId(iris_entity_id);
                            out_vertex.SetIrisMidUV(quad_attr.mid_uv);
                            out_vertex.SetIrisTangent(tangent);
                        }
                    }
                }
                vertex_writer.EndQuad();

                if constexpr (kCulling) {
                    if (processed_quads == quad_count_after_culling) {
                        break;
                    }
                }
            }  // quad
        }  // cube
    }  // cube group
    return RenderResult::kSuccess;
}
}
YSM_FAST_MATH_END
