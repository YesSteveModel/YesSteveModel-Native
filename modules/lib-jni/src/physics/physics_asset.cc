#include <java/buffer.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>
#include <physics/physics_asset.h>

namespace ysm::lib::physics {
YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/physics/NativePhysicsAsset;nCompile(Ljava/lang/Object;JLjava/lang/Object;J)J",
    (geometry, geometry_flags, definition, definition_flags)) {
    YSM_DECLARE_OR_RETURN(model, java::BufferInput<true, true>::Get(env, geometry, geometry_flags));
    YSM_DECLARE_OR_RETURN(physics, java::BufferInput<true, true>::Get(env, definition, definition_flags));
    YSM_DECLARE_OR_RETURN(asset, ysm::physics::CompilePhysicsAsset(model, physics));
    return java::ShareOpaquePtr(asset);
}
}  // namespace ysm::lib::physics
