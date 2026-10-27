#ifndef HEADER_TRANSFORM_COMMON_IMPL
#define HEADER_TRANSFORM_COMMON_IMPL

#include <cmath>
#include <cstdint>

#include <cglm/mat3.h>
#include <cglm/mat4.h>
#include <cglm/vec3.h>
#include <cglm/vec4.h>

#include "fast_math.h"
#include "inline.h"

#include "renderer/render_state.h"
#include "renderer/buffer/normal.h"
#include "output.h"

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::renderer::cube::internal {
YSM_INLINE void NormalizeDirectionScalar(vec3 direction) noexcept {
    const auto length_squared = glm_vec3_norm2(direction);
    if (length_squared > 0.0f && std::isfinite(length_squared)) {
        glm_vec3_scale(direction, 1.0f / std::sqrt(length_squared), direction);
    } else {
        glm_vec3_zero(direction);
    }
}
}

YSM_FAST_MATH_END

#endif

YSM_FAST_MATH_BEGIN

namespace ysm::gfx::renderer::cube::internal {
template <bool kCulling, bool kIris, bool kHasPbr, bool kPosOnly,
          typename CubeGroupType>
YSM_SIMD_TARGET
YSM_INLINE void TransformQuadAttributes(YSM_SIMD_TAG tag,
    const CubeGroupType& cube_group, const RenderBoneState& state,
    CubeOutput<CubeGroupType>& out, uint32_t attr_index) {
    vec4 face_plane{cube_group.normal[0][attr_index],
                    cube_group.normal[1][attr_index],
                    cube_group.normal[2][attr_index],
                    cube_group.plane_d[attr_index]};
    const auto facing = glm_vec4_dot(face_plane, state.facing_coeff) *
                        cube_group.winding_sign[attr_index];
    const bool back_face =
        (out.back_face[attr_index] = facing <= 0.0f);

    if constexpr (kCulling) {
        if (back_face) {
            return;
        }
    }

    vec4 normal;
    glm_mat3_mulv(state.normal, face_plane, normal);
    if (!state.uniform_scale) {
        NormalizeDirectionScalar(normal);
    }
    if (back_face) {
        glm_vec3_scale(normal, -1, normal);
    }
    out.normal[attr_index] = buffer::PackSnorm4x8<true>(tag, normal);

    if constexpr (kIris && kHasPbr && !kPosOnly) {
        vec3 source_tangent{cube_group.tangent[0][attr_index],
                            cube_group.tangent[1][attr_index],
                            cube_group.tangent[2][attr_index]};
        vec4 tangent{};
        if (!state.uniform_scale) {
            glm_mat4_mulv3(state.pose, source_tangent, 0.0f, tangent);
            NormalizeDirectionScalar(tangent);
        } else {
            glm_mat3_mulv(state.normal, source_tangent, tangent);
        }
        const auto baked_handedness = cube_group.tangent[3][attr_index];
        if (baked_handedness == 0.0f) {
            tangent[3] = 1.0f;
        } else {
            tangent[3] = baked_handedness * state.tangent_orientation;
            if (back_face) {
                tangent[3] = -tangent[3];
            }
        }
        out.tangent[attr_index] = buffer::PackSnorm4x8<false>(tag, tangent);
    }

    if constexpr (CubeGroupType::kTranslucent) {
        vec4 center{cube_group.center[0][attr_index],
                    cube_group.center[1][attr_index],
                    cube_group.center[2][attr_index], 1.0f};
        const auto clip_z = glm_vec4_dot(center, state.depth_z);
        const auto clip_w = glm_vec4_dot(center, state.depth_w);
        out.face_depth[attr_index] = clip_z / clip_w;
    }
}

}  // namespace ysm::gfx::renderer::cube::internal

YSM_FAST_MATH_END
