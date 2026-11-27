#include <physics/runtime.h>

#include <mutex>
#include <string_view>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/PhysicsMaterialSimple.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

namespace ysm::physics {

void InitializeRuntime() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
#ifdef JPH_ENABLE_ASSERTS
        // Update reports capacity loss both as an assertion and a recoverable return
        // value. Preserve every other engine assertion, including joint setup errors.
        static const auto previous_assert = JPH::AssertFailed;
        JPH::AssertFailed = [](const char* expression, const char* message, const char* file, JPH::uint line) {
            if (std::string_view(expression) == "errors == EPhysicsUpdateError::None") {
                return false;
            }
            return previous_assert(expression, message, file, line);
        };
#endif
        JPH::RegisterDefaultAllocator();
        JPH::CollisionDispatch::sInit();

        // The base classes fill the collision and cast tables without registering shape constructors.
        JPH::CompoundShape::sRegister();
        JPH::ConvexShape::sRegister();
        JPH::SphereShape::sRegister();
        JPH::BoxShape::sRegister();
        JPH::CapsuleShape::sRegister();
        JPH::StaticCompoundShape::sRegister();
        JPH::ScaledShape::sRegister();

        JPH::PhysicsMaterial::sDefault = new JPH::PhysicsMaterialSimple("Default", JPH::Color::sGrey);
    });
}

}  // namespace ysm::physics
