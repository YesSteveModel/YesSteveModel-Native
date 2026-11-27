#include "physics_asset_internal.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/str_cat.h>

#include "model/bone_hierarchy.h"
#include "native_joint.h"
#include "proto/asset/model/data/geo_model.proto.h"
#include "single_track_joint_constraint.h"

namespace ysm::physics {
namespace {
JPH::Vec3 Vector(const auto& v) {
    return {v[0], v[1], v[2]};
}

bool Finite(JPH::Vec3Arg v) {
    return std::isfinite(v.GetX()) && std::isfinite(v.GetY()) && std::isfinite(v.GetZ());
}

bool Finite(const RigidPose& pose) {
    return Finite(pose.position) && Finite(pose.rotation.GetXYZ()) &&
           std::isfinite(pose.rotation.GetW());
}

JPH::Quat EulerZYX(const std::array<float, 3>& v) {
    return JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), v[2]) *
           JPH::Quat::sRotation(JPH::Vec3::sAxisY(), v[1]) *
           JPH::Quat::sRotation(JPH::Vec3::sAxisX(), v[0]);
}

JPH::SpringSettings ReadSpring(const schema::SpringParameters& spring) {
    if (const auto* frequency = std::get_if<schema::FrequencyDamping>(&spring.mode)) {
        return {JPH::ESpringMode::FrequencyAndDamping, frequency->frequency, *frequency->damping_ratio};
    }
    const auto& coefficients = std::get<schema::StiffnessDamping>(spring.mode);
    return {JPH::ESpringMode::StiffnessAndDamping, *coefficients.stiffness, *coefficients.damping};
}

Axis ReadAxis(const std::optional<schema::AxisConfig>& input, float units) {
    Axis axis;
    if (input) {
        axis.limit = input->limit.value_or(schema::LimitKind::kFree);
        axis.lower = input->min.value_or(0.0f) * units;
        axis.upper = input->max.value_or(0.0f) * units;
        if (input->spring) {
            axis.target = input->spring->target;
            if (input->spring->parameters) {
                axis.target_spring = ReadSpring(*input->spring->parameters);
            }
        }
        if (input->soft_limit_spring) {
            axis.soft_spring = ReadSpring(*input->soft_limit_spring);
        }
    }
    return axis;
}

bool IsEffectiveMechanical(const Constraint& constraint) {
    return HasNativeJoint(constraint) || SingleTrackJointConstraint::IsNeeded(constraint);
}

void SetAnchorDistances(std::span<Node> nodes, std::span<const Constraint> constraints,
                        size_t dynamic_count) {
    std::array<uint8_t, kMaxNodes> queue;
    size_t end = 0;
    for (size_t i = dynamic_count; i < nodes.size(); ++i) {
        nodes[i].anchor_distance = 0;
        queue[end++] = static_cast<uint8_t>(i);
    }
    for (size_t head = 0; head < end; ++head) {
        const auto node = queue[head];
        for (const auto edge : nodes[node].incident_constraints) {
            const auto& def = constraints[edge];
            const auto [a, b] = def.nodes;
            const auto other = a == node ? b : a;
            if (def.is_effective_mechanical && nodes[other].anchor_distance == UINT8_MAX) {
                nodes[other].anchor_distance = nodes[node].anchor_distance + 1;
                queue[end++] = other;
            }
        }
    }
}

RigidPose ReadFrame(const schema::ConstraintFrame& frame) {
    const auto x = Vector(frame.axis_x).Normalized();
    const auto z = x.Cross(Vector(frame.axis_y)).Normalized();
    const auto y = z.Cross(x);
    const JPH::Mat44 matrix(JPH::Vec4(x, 0), JPH::Vec4(y, 0), JPH::Vec4(z, 0),
                            JPH::Vec4(0, 0, 0, 1));
    return {Vector(frame.pivot_offset) * kPixelToMeter, matrix.GetQuaternion().Normalized()};
}
}  // namespace

CollectionGroupFilter::CollectionGroupFilter(std::span<const Constraint> constraints) {
    for (const auto& constraint : constraints) {
        if (constraint.disable_collisions) {
            const auto [a, b] = constraint.nodes;
            excluded_[a][b / 64] |= uint64_t{1} << (b % 64);
            excluded_[b][a / 64] |= uint64_t{1} << (a % 64);
        }
    }
}

bool CollectionGroupFilter::CanCollide(const JPH::CollisionGroup& a,
                                       const JPH::CollisionGroup& b) const {
    // Each entity has its own system, leaving GroupID available for the collection mask.
    const auto other = b.GetSubGroupID();
    const auto excluded = excluded_[a.GetSubGroupID()][other / 64] >> (other % 64);
    return ((a.GetGroupID() & b.GetGroupID()) != 0) & ((excluded & 1) == 0);
}

YSM_PIMPL_DEFINITION(PhysicsAsset)
PhysicsAsset::PhysicsAsset() : YSM_PIMPL_CONSTRUCT() {}
PhysicsAsset::~PhysicsAsset() = default;

absl::StatusOr<std::shared_ptr<const PhysicsAsset>> CompilePhysicsAsset(
    BufferViewR geometry, BufferViewR physics) {
    schema::GeoModel geo;
    schema::PhysicsDefinition definition;
    try {
        geo.from_pb(BufStr(geometry));
        struct_pb::from_pb(definition, BufStr(physics));
    } catch (const std::exception& error) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid physics asset protobuf: ", error.what()));
    }
    if (definition.nodes.size() > kMaxNodes || definition.constraints.size() > kMaxConstraints) {
        return absl::ResourceExhaustedError("Physics definition exceeds per-world capacity.");
    }
    size_t shape_count = 0;
    for (const auto& node : definition.nodes) {
        shape_count += node.shapes.size();
        if (node.shapes.size() > kMaxShapesPerNode || shape_count > kMaxShapesPerAsset) {
            return absl::ResourceExhaustedError("Physics definition exceeds shape capacity.");
        }
    }
    auto hierarchy = model::BuildBoneHierarchy(geo);
    if (!hierarchy.ok()) {
        return hierarchy.status();
    }

    auto output = std::shared_ptr<PhysicsAsset>(new PhysicsAsset);
    auto& asset = output->Pimpl();
    if (!definition.gravity.empty()) {
        asset.gravity = Vector(definition.gravity);
    }
    absl::flat_hash_map<std::string_view, uint16_t> sorted_by_name;
    sorted_by_name.reserve(geo.bones.size());
    for (uint32_t sorted = 0; sorted < geo.bones.size(); ++sorted) {
        const auto authored = hierarchy->sorted_bone_indices[sorted];
        sorted_by_name.emplace(geo.bones[authored].name, sorted);
    }

    std::vector<uint16_t> node_bones;
    node_bones.reserve(definition.nodes.size());
    std::vector<bool> included(geo.bones.size());
    for (const auto& node : definition.nodes) {
        const auto sorted = sorted_by_name.at(*node.bone);
        node_bones.push_back(sorted);
        for (uint32_t ancestor = sorted; ancestor != kNoBone && !included[ancestor];
             ancestor = hierarchy->list[ancestor].parent_index) {
            included[ancestor] = true;
        }
    }
    std::vector<uint32_t> fk_by_sorted(geo.bones.size(), kNoBone);
    std::vector<RigidPose> binding;
    for (uint32_t sorted = 0; sorted < geo.bones.size(); ++sorted) {
        if (!included[sorted]) {
            continue;
        }
        const auto& info = hierarchy->list[sorted];
        const auto parent = info.parent_index == kNoBone ? kNoBone : fk_by_sorted[info.parent_index];
        const auto pivot = Vector(info.pivot) * kPixelToMeter;
        const auto rotation = EulerZYX(info.bind_rotation);
        const auto pose = parent == kNoBone
            ? RigidPose{pivot, rotation}
            : ComposePose(binding[parent], {pivot - asset.fk_bones[parent].pivot, rotation});
        if (!Finite(pose)) {
            return absl::InvalidArgumentError("Physics bind transform exceeds solver range.");
        }
        fk_by_sorted[sorted] = static_cast<uint32_t>(binding.size());
        asset.fk_bones.push_back({static_cast<uint16_t>(sorted), kNoNode, parent, pivot});
        binding.push_back({pose.position, pose.rotation.Normalized()});
    }

    std::vector<uint32_t> ordered_nodes(definition.nodes.size());
    std::iota(ordered_nodes.begin(), ordered_nodes.end(), 0);
    std::ranges::stable_sort(ordered_nodes, [&](uint32_t a, uint32_t b) {
        const auto kind_a = definition.nodes[a].kind;
        const auto kind_b = definition.nodes[b].kind;
        return kind_a == kind_b ? node_bones[a] < node_bones[b] : kind_a < kind_b;
    });
    absl::flat_hash_map<std::string_view, uint8_t> node_id;
    node_id.reserve(ordered_nodes.size());
    asset.nodes.reserve(ordered_nodes.size());
    for (const auto index : ordered_nodes) {
        const auto& source = definition.nodes[index];
        const auto anchor = source.kind == schema::PhysicsNodeKind::kAnchor;
        node_id.emplace(source.id, static_cast<uint8_t>(asset.nodes.size()));
        auto& node = asset.nodes.emplace_back(Node{
            .id = source.id,
            .fk_index = fk_by_sorted[node_bones[index]],
            .kind = source.kind,
            .collection_mask = anchor ? 0u : source.collection_mask.value_or(1),
            .disable_when_hidden = source.disable_when_hidden.value_or(false),
            .authored_index = static_cast<uint8_t>(index),
            .mass = source.mass.value_or(1.0f),
            .friction = source.friction.value_or(0.5f),
            .restitution = source.bounciness.value_or(0.0f),
            .linear_damping = source.linear_damping.value_or(0.04f),
            .angular_damping = source.angular_damping.value_or(0.1f),
            .gravity_factor = source.gravity_scale.value_or(1.0f),
        });
        if (!anchor) {
            if (auto status = CompileNodeShape(source, node); !status.ok()) {
                return status;
            }
        }
        asset.dynamic_count += source.kind == schema::PhysicsNodeKind::kDynamic;
    }
    struct DynamicAncestor {
        uint32_t subtree_end;
        uint8_t slot;
    };
    std::array<DynamicAncestor, kMaxNodes> ancestors;
    size_t depth = 0;
    for (size_t slot = 0; slot < asset.dynamic_count; ++slot) {
        auto& node = asset.nodes[slot];
        auto& bone = asset.fk_bones[node.fk_index];
        while (depth && bone.sorted_index >= ancestors[depth - 1].subtree_end) {
            --depth;
        }
        bone.dynamic_slot = static_cast<uint8_t>(slot);
        node.dynamic_parent = depth ? ancestors[depth - 1].slot : kNoNode;
        ancestors[depth++] = {hierarchy->list[bone.sorted_index].subtree_end, bone.dynamic_slot};
    }

    asset.constraints.reserve(definition.constraints.size());
    for (const auto& source : definition.constraints) {
        const auto a = node_id.at(source.node_a);
        const auto b = node_id.at(source.node_b);
        const std::array frames = {ReadFrame(source.frame_a), ReadFrame(source.frame_b)};
        const auto rest = RelativePose(ComposePose(binding[asset.nodes[a].fk_index], frames[0]),
                                       ComposePose(binding[asset.nodes[b].fk_index], frames[1]));
        if (!Finite(rest)) {
            return absl::InvalidArgumentError(absl::StrCat("Physics joint pose out of range for ", source.id));
        }
        Axis cone;
        if (source.cone) {
            cone.limit = source.cone->limit;
            cone.upper = *source.cone->half_angle;
            if (source.cone->soft_limit_spring) {
                cone.soft_spring = ReadSpring(*source.cone->soft_limit_spring);
            }
        }
        const auto index = static_cast<uint16_t>(asset.constraints.size());
        asset.nodes[a].incident_constraints.push_back(index);
        asset.nodes[b].incident_constraints.push_back(index);
        auto& constraint = asset.constraints.emplace_back(Constraint{
            .id = source.id,
            .nodes = {a, b},
            .frames = frames,
            .kind = source.kind,
            .disable_collisions = source.disable_collisions.value_or(true),
            .rest_target = rest,
            .axes = {ReadAxis(source.linear_x, kPixelToMeter), ReadAxis(source.linear_y, kPixelToMeter),
                     ReadAxis(source.linear_z, kPixelToMeter), ReadAxis(source.angular_x, 1),
                     ReadAxis(source.angular_y, 1), ReadAxis(source.angular_z, 1)},
            .cone = cone,
        });
        constraint.is_effective_mechanical = IsEffectiveMechanical(constraint);
    }
    SetAnchorDistances(asset.nodes, asset.constraints, asset.dynamic_count);
    asset.group_filter = new CollectionGroupFilter(asset.constraints);
    return output;
}
}  // namespace ysm::physics
