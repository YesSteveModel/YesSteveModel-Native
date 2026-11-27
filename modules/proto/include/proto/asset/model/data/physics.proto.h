#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <ylt/struct_pb.hpp>

namespace ysm::proto::asset::model::data {

enum class PhysicsNodeKind : int32_t {
    kUnspecified = 0,
    kDynamic = 1,
    kProxy = 2,
    kAnchor = 3,
};

enum class PhysicsConstraintKind : int32_t {
    kUnspecified = 0,
    kFixed = 1,
    kPoint = 2,
    kHinge = 3,
    kGeneric = 4,
};

enum class LimitKind : int32_t {
    kUnspecified = 0,
    kFree = 1,
    kLocked = 2,
    kHard = 3,
    kSoft = 4,
};

enum class SpringTarget : int32_t {
    kUnspecified = 0,
    kOff = 1,
    kRestPose = 2,
    kAnimation = 3,
};

struct SphereShape {
    std::vector<float> center;
    float radius = 0;
};
YLT_REFL(SphereShape, center, radius);

struct CapsuleShape {
    std::vector<float> point_a;
    std::vector<float> point_b;
    float radius = 0;
};
YLT_REFL(CapsuleShape, point_a, point_b, radius);

struct BoxShape {
    std::vector<float> center;
    std::vector<float> size;
    std::vector<float> rotation;
};
YLT_REFL(BoxShape, center, size, rotation);

struct CollisionShape {
    // struct_pb uses the alternative index to select consecutive wire fields.
    std::variant<SphereShape, CapsuleShape, BoxShape> shape;
};
YLT_REFL(CollisionShape, shape);

struct FrequencyDamping {
    float frequency = 0;
    std::optional<float> damping_ratio;
};
YLT_REFL(FrequencyDamping, frequency, damping_ratio);

struct StiffnessDamping {
    std::optional<float> stiffness;
    std::optional<float> damping;
};
YLT_REFL(StiffnessDamping, stiffness, damping);

struct SpringParameters {
    std::variant<FrequencyDamping, StiffnessDamping> mode;
};
YLT_REFL(SpringParameters, mode);

struct SpringConfig {
    SpringTarget target = SpringTarget::kUnspecified;
    std::optional<SpringParameters> parameters;
};
YLT_REFL(SpringConfig, target, parameters);

struct AxisConfig {
    std::optional<LimitKind> limit;
    std::optional<float> min;
    std::optional<float> max;
    std::optional<SpringConfig> spring;
    std::optional<SpringParameters> soft_limit_spring;
};
YLT_REFL(AxisConfig, limit, min, max, spring, soft_limit_spring);

struct ConeConfig {
    LimitKind limit = LimitKind::kUnspecified;
    std::optional<float> half_angle;
    std::optional<SpringParameters> soft_limit_spring;
};
YLT_REFL(ConeConfig, limit, half_angle, soft_limit_spring);

struct ConstraintFrame {
    std::vector<float> pivot_offset;
    std::vector<float> axis_x;
    std::vector<float> axis_y;
    std::vector<float> axis_z;
};
YLT_REFL(ConstraintFrame, pivot_offset, axis_x, axis_y, axis_z);

struct PhysicsConstraint {
    std::string id;
    std::string node_a;
    std::string node_b;
    PhysicsConstraintKind kind = PhysicsConstraintKind::kUnspecified;
    ConstraintFrame frame_a;
    ConstraintFrame frame_b;
    std::optional<bool> disable_collisions;
    std::optional<AxisConfig> linear_x;
    std::optional<AxisConfig> linear_y;
    std::optional<AxisConfig> linear_z;
    std::optional<AxisConfig> angular_x;
    std::optional<AxisConfig> angular_y;
    std::optional<AxisConfig> angular_z;
    std::optional<ConeConfig> cone;
    std::optional<std::string> preset_tag;
};
YLT_REFL(PhysicsConstraint, id, node_a, node_b, kind, frame_a, frame_b, disable_collisions,
         linear_x, linear_y, linear_z, angular_x, angular_y, angular_z, cone, preset_tag);

struct PhysicsNode {
    std::string id;
    std::optional<std::string> bone;
    PhysicsNodeKind kind = PhysicsNodeKind::kUnspecified;
    std::vector<CollisionShape> shapes;
    std::optional<uint32_t> collection_mask;
    std::optional<float> friction;
    std::optional<float> bounciness;
    std::optional<float> mass;
    std::optional<float> linear_damping;
    std::optional<float> angular_damping;
    std::optional<float> gravity_scale;
    std::optional<bool> disable_when_hidden;
    std::optional<std::string> preset_tag;
};
YLT_REFL(PhysicsNode, id, bone, kind, shapes, collection_mask, friction, bounciness, mass,
         linear_damping, angular_damping, gravity_scale, disable_when_hidden, preset_tag);

struct CollectionName {
    uint32_t slot = 0;
    std::string name;
};
YLT_REFL(CollectionName, slot, name);

struct PhysicsDefinition {
    std::vector<PhysicsNode> nodes;
    std::vector<PhysicsConstraint> constraints;
    std::vector<CollectionName> collections;
    std::vector<float> gravity;
};
YLT_REFL(PhysicsDefinition, nodes, constraints, collections, gravity);

}  // namespace ysm::proto::asset::model::data
