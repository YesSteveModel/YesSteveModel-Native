#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>

#include "physics/physics_asset.h"
#include "proto/asset/model/data/physics.proto.h"

namespace ysm::physics {
namespace schema = proto::asset::model::data;

inline constexpr unsigned kMaxNodes = 128;
inline constexpr unsigned kMaxConstraints = 256;
inline constexpr unsigned kMaxShapesPerNode = 32;
inline constexpr unsigned kMaxShapesPerAsset = 512;
inline constexpr float kPixelToMeter = 1.0f / 16.0f;
inline constexpr uint32_t kNoBone = UINT32_MAX;
inline constexpr uint8_t kNoNode = UINT8_MAX;

struct RigidPose {
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();
};

struct FkBone {
    uint16_t sorted_index;
    uint8_t dynamic_slot;
    uint32_t parent;
    JPH::Vec3 pivot;
};

struct Node {
    std::string id;
    uint32_t fk_index;
    schema::PhysicsNodeKind kind;
    uint32_t collection_mask;
    bool disable_when_hidden;
    uint8_t dynamic_parent = kNoNode;
    uint8_t anchor_distance = UINT8_MAX;
    uint8_t authored_index = 0;
    JPH::RefConst<JPH::Shape> shape;
    RigidPose shape_from_bone;
    JPH::Vec3 com_in_bone = JPH::Vec3::sZero();
    JPH::Vec3 base_inv_inertia_diagonal = JPH::Vec3::sZero();
    // Principal axes in the body's shape-local coordinates, as consumed by SetInverseInertia.
    JPH::Quat base_inertia_rotation = JPH::Quat::sIdentity();
    float mass;
    float friction;
    float restitution;
    float linear_damping;
    float angular_damping;
    float gravity_factor;
    std::vector<uint16_t> incident_constraints;
};

struct Axis {
    schema::LimitKind limit = schema::LimitKind::kFree;
    float lower = 0;
    float upper = 0;
    schema::SpringTarget target = schema::SpringTarget::kOff;
    JPH::SpringSettings target_spring;
    JPH::SpringSettings soft_spring;
};

struct Constraint {
    std::string id;
    std::array<uint8_t, 2> nodes;
    std::array<RigidPose, 2> frames;
    schema::PhysicsConstraintKind kind;
    bool disable_collisions;
    bool is_effective_mechanical;
    RigidPose rest_target;
    std::array<Axis, 6> axes;
    Axis cone;
};

absl::Status CompileNodeShape(const schema::PhysicsNode& source, Node& node);

class CollectionGroupFilter final : public JPH::GroupFilter {
public:
    explicit CollectionGroupFilter(std::span<const Constraint> constraints);
    bool CanCollide(const JPH::CollisionGroup& a, const JPH::CollisionGroup& b) const override;

private:
    std::array<std::array<uint64_t, 2>, kMaxNodes> excluded_{};
};

class PhysicsAsset::Impl {
public:
    std::vector<FkBone> fk_bones;
    std::vector<Node> nodes;
    std::vector<Constraint> constraints;
    JPH::Vec3 gravity = JPH::Vec3(0, -9.81f, 0);
    uint32_t dynamic_count = 0;
    JPH::RefConst<CollectionGroupFilter> group_filter;
};

inline RigidPose RelativePose(const RigidPose& a, const RigidPose& b) {
    const auto inverse = a.rotation.Conjugated();
    return {inverse * (b.position - a.position), inverse * b.rotation};
}

inline RigidPose ComposePose(const RigidPose& a, const RigidPose& b) {
    return {a.position + a.rotation * b.position, a.rotation * b.rotation};
}
}  // namespace ysm::physics
