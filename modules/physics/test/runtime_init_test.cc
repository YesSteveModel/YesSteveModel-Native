#include <physics/runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <gtest/gtest.h>
#include <mimalloc.h>

namespace ysm::physics {
namespace {

TEST(PhysicsRuntimeTest, RegistersOnlySupportedShapeConstructorsOnce) {
    InitializeRuntime();
    const auto* material = JPH::PhysicsMaterial::sDefault.GetPtr();
    ASSERT_NE(material, nullptr);
    InitializeRuntime();
    EXPECT_EQ(JPH::PhysicsMaterial::sDefault.GetPtr(), material);

    using ShapeType = JPH::EShapeSubType;
    constexpr std::array supported{ShapeType::Sphere, ShapeType::Box, ShapeType::Capsule,
                                   ShapeType::StaticCompound, ShapeType::Scaled};
    for (const auto type : JPH::sAllSubShapeTypes) {
        SCOPED_TRACE(static_cast<int>(type));
        EXPECT_EQ(JPH::ShapeFunctions::sGet(type).mConstruct != nullptr,
                  std::ranges::find(supported, type) != supported.end());
    }
}

TEST(PhysicsRuntimeTest, DispatchesConvexAndCompoundPairs) {
    InitializeRuntime();
    const JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(1.0f);
    const JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    JPH::StaticCompoundShapeSettings settings;
    settings.AddShape(JPH::Vec3(0.0f, 2.0f, 0.0f), JPH::Quat::sIdentity(), box.GetPtr());
    settings.AddShape(JPH::Vec3(0.0f, -2.0f, 0.0f), JPH::Quat::sIdentity(), box.GetPtr());
    const auto result = settings.Create();
    ASSERT_TRUE(result.IsValid());
    const JPH::RefConst<JPH::Shape> compound = result.Get();
    const JPH::RefConst<JPH::Shape> scaled =
        new JPH::ScaledShape(compound.GetPtr(), JPH::Vec3::sReplicate(2.0f));

    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> overlap;
    const auto unit_scale = JPH::Vec3::sReplicate(1.0f);
    const auto identity = JPH::Mat44::sIdentity();
    JPH::CollisionDispatch::sCollideShapeVsShape(sphere.GetPtr(), box.GetPtr(), unit_scale, unit_scale,
                                               identity, identity, {}, {}, {}, overlap);
    EXPECT_TRUE(overlap.HadHit());
    overlap.Reset();
    JPH::CollisionDispatch::sCollideShapeVsShape(scaled.GetPtr(), sphere.GetPtr(), unit_scale, unit_scale,
                                               identity, identity, {}, {}, {}, overlap);
    EXPECT_FALSE(overlap.HadHit());
    const auto end = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 4.0f, 0.0f));
    JPH::CollisionDispatch::sCollideShapeVsShape(scaled.GetPtr(), sphere.GetPtr(), unit_scale, unit_scale,
                                               identity, end, {}, {}, {}, overlap);
    EXPECT_TRUE(overlap.HadHit());

    const JPH::ShapeCast cast(compound.GetPtr(), unit_scale, identity, JPH::Vec3(0.0f, 4.0f, 0.0f));
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hit;
    JPH::CollisionDispatch::sCastShapeVsShapeWorldSpace(cast, {}, sphere.GetPtr(), unit_scale,
                                                     {}, identity, {}, {}, hit);
    ASSERT_TRUE(hit.HadHit());
    EXPECT_NEAR(hit.mHit.mFraction, 0.125f, 1e-3f);
}

TEST(PhysicsRuntimeTest, AllocatesAndReallocatesWithMimalloc) {
    InitializeRuntime();
    std::unique_ptr<void, JPH::FreeFunction> block(JPH::Allocate(32), JPH::Free);
    ASSERT_NE(block, nullptr);
    ASSERT_TRUE(mi_is_in_heap_region(block.get()));
    *static_cast<unsigned char*>(block.get()) = 0x5a;

    void* resized = JPH::Reallocate(block.get(), 32, 8192);
    ASSERT_NE(resized, nullptr);
    block.release();
    block.reset(resized);
    EXPECT_TRUE(mi_is_in_heap_region(block.get()));
    EXPECT_EQ(*static_cast<unsigned char*>(block.get()), 0x5a);
    EXPECT_EQ(JPH::Reallocate(block.get(), 8192, std::numeric_limits<size_t>::max()), nullptr);
    EXPECT_EQ(*static_cast<unsigned char*>(block.get()), 0x5a);

    constexpr size_t alignment = 64;
    const std::unique_ptr<void, JPH::AlignedFreeFunction> aligned(
        JPH::AlignedAllocate(113, alignment), JPH::AlignedFree);
    ASSERT_NE(aligned, nullptr);
    EXPECT_TRUE(mi_is_in_heap_region(aligned.get()));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned.get()) % alignment, 0u);
}

class SingleBroadPhaseLayer final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {
        return JPH::BroadPhaseLayer(0);
    }
};

TEST(PhysicsRuntimeTest, AppliesExponentialDampingAtTenHertz) {
    InitializeRuntime();
    const SingleBroadPhaseLayer broad_phase_layers;
    const JPH::ObjectVsBroadPhaseLayerFilter broad_phase_filter;
    const JPH::ObjectLayerPairFilter object_layer_filter;
    JPH::PhysicsSystem world;
    world.Init(16, 0, 32, 32, broad_phase_layers, broad_phase_filter, object_layer_filter);
    world.SetGravity(JPH::Vec3::sZero());
    const JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(1.0f);
    JPH::BodyCreationSettings settings(sphere.GetPtr(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Dynamic, 0);
    settings.mLinearVelocity = JPH::Vec3(1.0f, 0.0f, 0.0f);
    settings.mAngularVelocity = JPH::Vec3(0.0f, 1.0f, 0.0f);
    settings.mLinearDamping = 10.0f;
    settings.mAngularDamping = 10.0f;
    settings.mAllowSleeping = false;
    auto& bodies = world.GetBodyInterface();
    const auto body = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    ASSERT_FALSE(body.IsInvalid());
    JPH::TempAllocatorMalloc scratch;
    JPH::JobSystemSingleThreaded jobs(64);
    EXPECT_EQ(world.Update(0.1f, 1, &scratch, &jobs), JPH::EPhysicsUpdateError::None);
    EXPECT_NEAR(bodies.GetLinearVelocity(body).GetX(), std::exp(-1.0f), 1e-6f);
    EXPECT_NEAR(bodies.GetAngularVelocity(body).GetY(), std::exp(-1.0f), 1e-6f);
    bodies.RemoveBody(body);
    bodies.DestroyBody(body);
}

}  // namespace
}  // namespace ysm::physics
