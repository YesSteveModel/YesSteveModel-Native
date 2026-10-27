#pragma once

#include <memory>

#include "baked_model.h"
#include "buffer_managed.h"

namespace ysm::gfx::bake {
BufferManaged SerializeBakedModel(const BakedModel& model);

// Includes the owning output capacity; serialization has no model-sized scratch allocation.
size_t BakedModelSerializationBytes(const BakedModel& model);

absl::StatusOr<std::unique_ptr<BakedModel>> ReadBakedModel(
    BufferViewR baked_model_data);
}  // namespace ysm::gfx::bake
