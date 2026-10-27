#include <cstdint>

#include <gfx/bake/baked_model.h>
#include <gfx/bake/baked_serializer.h>
#include <bitpack.h>
#include <cpu.h>
#include <java/array.h>
#include <java/buffer.h>
#include <java/entry.h>
#include <log.h>

#include "java/opaque_ptr.h"
#include "model_result.h"

namespace ysm::lib::bake {
namespace {
YSM_BIT_PACK(PackedBakeOptions, jlong,
             YSM_BIT_FIELD(origin_ver, uint16_t, 16),
             YSM_BIT_FIELD(force_culling, uint8_t, 1),
             YSM_BIT_FIELD(force_translucent, uint8_t, 1),
             YSM_BIT_FIELD(has_pbr, uint8_t, 1));
}  // namespace

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nCapability()I",
    ()) {
    return static_cast<jint>(simd::kSupported);
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nBake(Ljava/lang/Object;J[SJIIJ)J",
    (model_data_buf, model_data_flags, sorted_bone_indices_array, pixels_ptr,
     pixels_width, pixels_height, bake_options)) {
    YSM_DECLARE_OR_RETURN(model_data,
                          java::BufferInput<true, true>::Get(
                              env, model_data_buf, model_data_flags));
    if (pixels_ptr == 0 || pixels_width <= 0 || pixels_height <= 0) {
        return absl::InvalidArgumentError("Invalid model texture.");
    }
    const auto [origin_ver, force_culling, force_translucent, has_pbr] =
        PackedBakeOptions::unpack(bake_options);
    ysm::gfx::bake::Texture texture(reinterpret_cast<const ysm::gfx::bake::Pixel*>(pixels_ptr),
                          static_cast<size_t>(pixels_width),
                          static_cast<size_t>(pixels_height));
    YSM_DECLARE_OR_RETURN(
        baked_model,
        ysm::gfx::bake::BakeModel(model_data, texture,
                             {.origin_ver = origin_ver,
                              .force_culling = force_culling != 0,
                              .force_translucent = force_translucent != 0,
                              .has_pbr = has_pbr != 0}));
    YSM_LOG_DEBUG(
        "Baked model: input={} bytes, texture={}x{}, bones={}",
        model_data.size(), pixels_width, pixels_height,
        baked_model->Bones().sorted_bone_indices.size());

    return DeliverModel(env, std::move(baked_model), sorted_bone_indices_array);
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nStorageBytes(J)J",
    (baked_model_ptr)) {
    YSM_DECLARE_OR_RETURN(
        model, java::CastOpaquePtr<ysm::gfx::bake::BakedModel>(baked_model_ptr));
    return static_cast<jlong>(model->CapacityBytes());
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nSerializationBytes(J)J",
    (baked_model_ptr)) {
    YSM_DECLARE_OR_RETURN(
        model, java::CastOpaquePtr<ysm::gfx::bake::BakedModel>(baked_model_ptr));
    return static_cast<jlong>(ysm::gfx::bake::BakedModelSerializationBytes(*model));
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nSerialize(J)Ljava/nio/ByteBuffer;",
    (baked_model_ptr)) {
    YSM_DECLARE_OR_RETURN(
        model, java::CastOpaquePtr<ysm::gfx::bake::BakedModel>(baked_model_ptr));
    auto data = ysm::gfx::bake::SerializeBakedModel(*model);
    return java::TryMoveToOutput(env, std::move(data), java::BufferType::kDirect);
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nTryBake(Ljava/lang/Object;JJIIJ)Z",
    (model_data_buf, model_data_flags, pixels_ptr, pixels_width, pixels_height,
     bake_options)) {
    YSM_DECLARE_OR_RETURN(model_data,
                          java::BufferInput<true, true>::Get(
                              env, model_data_buf, model_data_flags));
    if (pixels_ptr == 0 || pixels_width <= 0 || pixels_height <= 0) {
        return absl::InvalidArgumentError("Invalid model texture.");
    }
    const auto [origin_ver, force_culling, force_translucent, has_pbr] =
        PackedBakeOptions::unpack(bake_options);
    ysm::gfx::bake::Texture texture(reinterpret_cast<const ysm::gfx::bake::Pixel*>(pixels_ptr),
                          static_cast<size_t>(pixels_width),
                          static_cast<size_t>(pixels_height));
    return ysm::gfx::bake::TryBakeModel(model_data, texture,
                                   {.origin_ver = origin_ver,
                                    .force_culling = force_culling != 0,
                                    .force_translucent = force_translucent != 0,
                                    .has_pbr = has_pbr != 0});
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/render/NativeBakedModel;nRead(Ljava/lang/Object;J[S)J",
    (baked_model_buf_obj, baked_model_buf_flags, sorted_bone_indices_array)) {
    YSM_DECLARE_OR_RETURN(
        baked_model_buf,
        java::BufferInput<true, true>::Get(env, baked_model_buf_obj,
                                           baked_model_buf_flags));
    YSM_DECLARE_OR_RETURN(model, ysm::gfx::bake::ReadBakedModel(baked_model_buf));
    YSM_LOG_DEBUG("Read baked model: input={} bytes, bones={}",
                  baked_model_buf.size(),
                  model->Bones().sorted_bone_indices.size());
    return DeliverModel(env, std::move(model), sorted_bone_indices_array);
}
}  // namespace ysm::lib::bake
