#pragma once

#include <cglm/mat4.h>

#include "cpu.h"
#include "fast_math.h"

#include "gfx/bake/baked_model.h"
#include "renderer/render_state.h"
#include "output.h"
#include "transform_common.h"

#ifdef YSM_X64
#include "transform_avx2.h"
#include "transform_avx512.h"
#include "transform_sse41.h"
#elif (defined YSM_ARM64)
#include "transform_neon.h"
#endif

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::renderer::cube {

template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
void Transform(simd::GenericTag, const CubeGroupType& cube_group,
               const RenderBoneState& state, CubeOutput<CubeGroupType>& out) {
    const auto cube_count =
        CubeGroupType::kCubeGroupCapacity > 1 ? cube_group.cube_count : 1U;

    const auto vertex_count = cube_group.IsSingleQuad() ? 4U : cube_count * 8U;
    for (uint32_t vertex_index = 0; vertex_index < vertex_count;
         ++vertex_index) {
        vec3 position{cube_group.pos[0][vertex_index],
                      cube_group.pos[1][vertex_index],
                      cube_group.pos[2][vertex_index]};
        vec3 transformed;
        glm_mat4_mulv3(state.pose, position, 1.0f, transformed);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            out.pos[axis][vertex_index] = transformed[axis];
        }
    }

    for (uint32_t cube_index = 0; cube_index < cube_count; ++cube_index) {
        const auto quad_count = cube_group.cube_attr[cube_index].quad_count;
        for (uint32_t quad_index = 0; quad_index < quad_count; ++quad_index) {
            const auto attr_index =
                CubeGroupType::GetQuadAttrIndex(cube_index, quad_index);
            internal::TransformQuadAttributes<
                kCulling, kIris, kHasPbr, kPosOnly>(
                simd::GenericTag{}, cube_group, state, out, attr_index);
        }
    }
}
}  // namespace ysm::gfx::renderer::cube

YSM_FAST_MATH_END
