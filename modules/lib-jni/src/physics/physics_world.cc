#include <vector>

#include <java/array.h>
#include <java/entry.h>
#include <java/opaque_ptr.h>
#include <physics/physics_world.h>

namespace ysm::lib::physics {
YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/physics/NativePhysicsWorld;nCreate(J)J",
    (asset_ptr)) {
    YSM_DECLARE_OR_RETURN(asset, java::CastOpaquePtr<const ysm::physics::PhysicsAsset>(asset_ptr));
    YSM_DECLARE_OR_RETURN(world, ysm::physics::PhysicsWorld::Create(asset));
    return java::MakeOpaquePtr<ysm::physics::PhysicsWorld>(std::move(world));
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/physics/NativePhysicsWorld;nSetDebugContactCapture(JZ)V",
    (world_ptr, enabled)) {
    YSM_DECLARE_OR_RETURN(world, java::CastOpaquePtr<ysm::physics::PhysicsWorld>(world_ptr));
    world->SetDebugContactCapture(enabled != JNI_FALSE);
    return OkStatus();
}

YSM_JNI_ENTRY(
    "Lcom/elfmcys/ysm/natives/physics/NativePhysicsWorld;nReadDebugSnapshot(J[F[I)I",
    (world_ptr, float_out, int_out)) {
    YSM_DECLARE_OR_RETURN(owner, java::CastOpaquePtr<ysm::physics::PhysicsWorld>(world_ptr));
    const auto& world = *owner;
    const auto [float_count, int_count] = world.DebugSnapshotLayout();
    YSM_ASSERT(float_out != nullptr && int_out != nullptr &&
                   env->GetArrayLength(float_out) == static_cast<jsize>(float_count) &&
                   env->GetArrayLength(int_out) == static_cast<jsize>(int_count),
               absl::InvalidArgumentError("Invalid physics debug snapshot buffers."));
    if (!world.HasSample()) {
        return jint{0};
    }
    // Stage outside the JVM arrays so reading the world never pins the heap.
    std::vector<jfloat> floats(float_count);
    std::vector<jint> ints(int_count);
    const auto flags = world.ReadDebugSnapshot({floats, ints});
    YSM_RETURN_IF_ERROR(java::WriteFloatArray<true>(env, float_out, floats));
    YSM_ASSERT(!env->ExceptionCheck(), absl::InternalError("Failed to publish physics debug coordinates."));
    YSM_RETURN_IF_ERROR(java::WriteIntArray<true>(env, int_out, ints));
    YSM_ASSERT(!env->ExceptionCheck(), absl::InternalError("Failed to publish physics debug states."));
    return static_cast<jint>(flags);
}
}  // namespace ysm::lib::physics
