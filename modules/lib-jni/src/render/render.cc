#include "gfx/renderer/model_state.h"
#include "gfx/renderer/render.h"

#include "bitpack.h"
#include "java/buffer.h"
#include "java/entry.h"
#include "java/opaque_ptr.h"
#include "log.h"
#include "scope_guard.h"

namespace ysm::lib::render {
namespace {
YSM_BIT_PACK(PackedRenderFlags, jlong,
             YSM_BIT_FIELD(context, gfx::renderer::RenderContext, 2),
             YSM_BIT_FIELD(vertex_kind, gfx::renderer::VertexKind, 16));
YSM_BIT_PACK(PackedLightAndOverlay, jlong,
             YSM_BIT_FIELD(overlay, uint32_t, 32),
             YSM_BIT_FIELD(light, uint32_t, 32));
}  // namespace

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeRenderer;nRender(Ljava/lang/Object;IJJIJIJJ)I",
    (vertex_buffer_obj, vertex_buffer_flag, mat_ptr, model_state_ptr,
     vertex_count, light_and_overlay, color, flags, iris_entity_id)) {
    YSM_DECLARE_OR_RETURN(
        model_state,
        java::CastOpaquePtr<gfx::renderer::ModelState>(model_state_ptr));
    YSM_ASSERT(model_state->IsValid() && vertex_count >= 0 &&
                   static_cast<uint32_t>(vertex_count) == model_state->Schedule().vertex_count,
               absl::FailedPreconditionError("ModelState publication count mismatch."));
    if (vertex_count == 0) {
        return static_cast<jint>(gfx::renderer::RenderResult::kSuccess);
    }
    YSM_RETURN_IF_NULL(vertex_buffer_obj);
    YSM_ASSERT(mat_ptr != 0,
               absl::InvalidArgumentError("Render matrices are null."));
    const auto* mat =
        reinterpret_cast<const float*>(static_cast<uintptr_t>(mat_ptr));

    const auto [context, vertex_kind] =
        PackedRenderFlags::unpack(flags);
    YSM_BIT_ENUM_VALIDATE(context, vertex_kind);
    BufferView vertex_buffer;
    // ReSharper disable once CppTooWideScope
    java::CriticalIntArray<false> vertex_buffer_array;
    if (vertex_buffer_flag > 0) {
        YSM_ASSIGN_OR_RETURN(
            vertex_buffer, java::GetDirectBuffer(env, vertex_buffer_obj));
    } else {
        YSM_ASSIGN_OR_RETURN(
            vertex_buffer_array,
            java::CriticalIntArray<false>::Get(
                env, reinterpret_cast<jintArray>(vertex_buffer_obj)));
        vertex_buffer = {
            reinterpret_cast<Byte*>(vertex_buffer_array.data()),
            vertex_buffer_array.size() * 4};
    }

    const auto [overlay, light] =
        PackedLightAndOverlay::unpack(light_and_overlay);
    gfx::renderer::RenderParameters parameters;
    glm_mat4_make(mat, parameters.model);
    glm_mat4_make(mat + 16, parameters.view);
    glm_mat4_make(mat + 32, parameters.projection);
    glm_mat3_make(mat + 48, parameters.normal);
    parameters.ctx = context;
    parameters.light = light;
    parameters.overlay = overlay;
    parameters.color =
        std::bit_cast<gfx::renderer::Color>(static_cast<uint32_t>(color));
    parameters.iris_entity_id = static_cast<uint64_t>(iris_entity_id);

    gfx::renderer::RenderResult result;
    const auto status = gfx::renderer::Render(vertex_buffer, vertex_kind, *model_state,
                                             parameters, result);
    if (!status.ok()) [[unlikely]] {
        YSM_LOG_DEBUG_EVERY_N_SEC(5, "Native render failed: {}", status.message());
    }
    return static_cast<jint>(result);
}
}  // namespace ysm::lib::render
