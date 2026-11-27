#include "physics_world_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "gfx/renderer/bone_attribute.h"

namespace ysm::physics {
namespace {
using gfx::renderer::BoneAttribute;
using Kind = DeactivationKind;

void EvaluateBone(WorkerScratch::FkState& state, const WorkerScratch::FkState& parent,
                  const FkBone& bone, JPH::Vec3Arg parent_pivot, const BoneAttribute& channels,
                  const RigidPose& sim_root, std::span<const Node> nodes) {
    state.hidden = {};
    if (parent.hidden_children != kNoBone) {
        state.hidden = {Kind::kChildrenHidden, static_cast<uint16_t>(parent.hidden_children)};
    } else if (channels.cubes_hidden != 0) {
        state.hidden = {Kind::kCubesHidden, bone.sorted_index};
    }
    state.hidden_children = channels.children_hidden != 0 ? bone.sorted_index : parent.hidden_children;
    state.invalid = parent.invalid;
    state.scale = parent.scale * channels.scale[0];
    if (state.invalid.kind == Kind::kActive) {
        if (channels.scale[0] != channels.scale[1] || channels.scale[0] != channels.scale[2] ||
            !(state.scale >= 1.0e-6f && state.scale <= 1.0e6f)) [[unlikely]] {
            state.invalid = {Kind::kInvalidScale, bone.sorted_index};
        }
    }
    if (state.invalid.kind != Kind::kActive) [[unlikely]] {
        state.animation = {};
        state.scale = 0;
        return;
    }

    const auto& t = channels.position;
    const auto& r = channels.rotation;
    const JPH::Vec3 euler(r[0], r[1], r[2]);
    if (NonFinite(JPH::Vec4(euler, 0)).TestAnyTrue()) [[unlikely]] {
        // Jolt's vectorized trig reduces the angle to an integer quadrant.
        state.animation = {};
        state.invalid = {Kind::kInvalidPose, bone.sorted_index};
        return;
    }
    const auto local_offset = bone.pivot - parent_pivot + kPixelToMeter * JPH::Vec3(-t[0], t[1], t[2]);
    // Animation publishes absolute local Euler channels, including the authored bind rotation.
    state.animation = ComposePose(parent.animation, {
        parent.scale * local_offset, JPH::Quat::sEulerAngles(euler)});
    state.animation.rotation = state.animation.rotation.Normalized();
    if (NonFinite(ComposePose(sim_root, state.animation)).TestAnyTrue() ||
        !state.animation.rotation.IsNormalized()) [[unlikely]] {
        state.animation = {};
        state.invalid = {Kind::kInvalidPose, bone.sorted_index};
    } else if (bone.dynamic_slot != kNoNode) {
        const auto inverse = nodes[bone.dynamic_slot].base_inv_inertia_diagonal / (state.scale * state.scale);
        if (NonFinite(JPH::Vec4(inverse, 0)).TestAnyTrue() || !(inverse.ReduceMin() > 0)) [[unlikely]] {
            state.invalid = {Kind::kInvalidInertia, bone.sorted_index};
        }
    }
}
}  // namespace

bool PhysicsWorld::PrepareGather() noexcept {
    try {
        WorkerScratch::Current().Prepare(impl_->data.fk_bones.size());
        return true;
    } catch (const std::bad_alloc&) {
        impl_->Fail(StepOutcome::kScratchExhausted);
        return false;
    }
}

void PhysicsWorld::GatherBoneAttributes(std::span<const float> attributes) noexcept {
    auto& gathered = WorkerScratch::Current().fk_bones;
    const auto& bones = impl_->data.fk_bones;
    static_assert(sizeof(WorkerScratch::BoneChannels) == sizeof(BoneAttribute));
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto offset = size_t{bones[i].sorted_index} * BoneAttribute::kFloatCount;
        std::memcpy(gathered[i].data(), attributes.data() + offset, sizeof(BoneAttribute));
    }
}

void PhysicsWorld::Impl::EvaluateBones(WorkerScratch& scratch, const RigidPose& sim_root, bool valid_root) {
    WorkerScratch::FkState model;
    for (size_t i = 0; i < data.fk_bones.size(); ++i) {
        const auto& bone = data.fk_bones[i];
        const bool is_root = bone.parent == kNoBone;
        if (!valid_root && is_root) [[unlikely]] {
            model.invalid = {Kind::kInvalidPose, bone.sorted_index};
        }
        EvaluateBone(scratch.fk_states[i], is_root ? model : scratch.fk_states[bone.parent], bone,
                     is_root ? JPH::Vec3::sZero() : data.fk_bones[bone.parent].pivot,
                     std::bit_cast<BoneAttribute>(scratch.fk_bones[i]), sim_root, data.nodes);
    }

    std::bitset<kMaxNodes> eligible;
    for (size_t i = 0; i < data.nodes.size(); ++i) {
        const auto& node = data.nodes[i];
        const auto& fk = scratch.fk_states[node.fk_index];
        targets[i].pivot = ToPose(fk.animation);
        targets[i].scale = fk.scale;
        const auto reason = fk.invalid.kind == Kind::kActive && node.disable_when_hidden ?
            fk.hidden : fk.invalid;
        eligible[i] = reason.kind == Kind::kActive;
        if (reason.kind != Kind::kActive) [[unlikely]] {
            reasons[i] = reason;
        }
    }
    if (!topology_known || eligible != direct_eligible) [[unlikely]] {
        UpdateAnchoring(eligible);
        direct_eligible = eligible;
        topology_known = true;
    }
}

void PhysicsWorld::Impl::UpdateAnchoring(const std::bitset<kMaxNodes>& eligible) {
    std::bitset<kMaxNodes> visited;
    std::array<uint8_t, kMaxNodes> queue;
    const auto flood = [&](size_t& end) {
        uint16_t cut = UINT16_MAX;
        uint8_t distance = UINT8_MAX;
        for (size_t head = 0; head < end; ++head) {
            const auto node = queue[head];
            for (const auto edge : data.nodes[node].incident_constraints) {
                const auto& def = data.constraints[edge];
                if (!def.is_effective_mechanical) continue;
                const auto [a, b] = def.nodes;
                const auto other = a == node ? b : a;
                if (!eligible[other]) {
                    // The closest boundary leads back toward an authored anchor.
                    // A disabled dead end may have a lower constraint index.
                    if (data.nodes[other].anchor_distance < distance) {
                        distance = data.nodes[other].anchor_distance;
                        cut = edge;
                    }
                } else if (!visited[other]) {
                    visited[other] = true;
                    queue[end++] = other;
                }
            }
        }
        return cut;
    };
    size_t end = 0;
    for (size_t i = data.dynamic_count; i < data.nodes.size(); ++i) {
        if (eligible[i]) {
            visited[i] = true;
            queue[end++] = static_cast<uint8_t>(i);
        }
    }
    flood(end);
    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i].active = visited[i];
        if (visited[i]) reasons[i] = {};
    }
    // Every admitted dynamic component had an anchor. A boundary edge of its
    // surviving component explains even nodes several links away from the cut.
    for (size_t i = 0; i < data.dynamic_count; ++i) {
        if (eligible[i] && !visited[i]) {
            visited[i] = true;
            queue[0] = static_cast<uint8_t>(i);
            end = 1;
            const auto cut = flood(end);
            for (size_t member = 0; member < end; ++member) {
                reasons[queue[member]] = {Kind::kLostAnchor, cut};
            }
        }
    }
}

StepResult PhysicsWorld::AdvanceGathered(bool reset, float dt, const std::array<double, 3>& root_position,
                                         const std::array<float, 4>& root_rotation) noexcept {
    auto& world = *impl_;
    PhysicsFrame frame{root_position, root_rotation, world.targets};
    const auto offset = reset ? JPH::Vec3::sZero() : RootOffset(root_position, world.origin);
    const RigidPose sim_root{NeedsRebase(offset) ? JPH::Vec3::sZero() : offset, Quaternion(root_rotation)};
    const bool valid_root = std::ranges::all_of(root_position, [](double p) { return std::isfinite(p); }) &&
                            !NonFinite(JPH::Vec4(offset, 0)).TestAnyTrue() && sim_root.rotation.IsNormalized();
    if (!valid_root) [[unlikely]] {
        // All nodes exit locally. Keep the floating origin usable by the next legal pose.
        frame.root_position = world.origin;
        frame.root_rotation = {0, 0, 0, 1};
    }
    world.EvaluateBones(WorkerScratch::Current(), sim_root, valid_root);
    return reset ? Reset(frame) : Step(frame, dt);
}

StepResult PhysicsWorld::Impl::Publish(bool stepped, size_t peak) {
    std::array<uint8_t, kMaxNodes> active_ancestor;
    auto invalid = JPH::UVec4::sZero();
    deltas.clear();
    for (size_t i = 0; i < data.dynamic_count; ++i) {
        const auto& node = data.nodes[i];
        const auto ancestor = node.dynamic_parent == kNoNode ? kNoNode : active_ancestor[node.dynamic_parent];
        active_ancestor[i] = ancestor;
        if (!targets[i].active) continue;

        auto animated = FromPose(targets[i].pivot);
        if (ancestor != kNoNode) {
            // The pass will already inherit its nearest active ancestor's correction.
            animated = ComposePose(solved_pivots[ancestor],
                                   RelativePose(FromPose(targets[ancestor].pivot), animated));
        }
        active_ancestor[i] = static_cast<uint8_t>(i);
        const auto solved = RelativePose(root, Pivot(i));
        solved_pivots[i] = solved;
        auto correction = RelativePose(animated, solved);
        correction.position /= targets[i].scale;
        correction.rotation = correction.rotation.Normalized();
        invalid = JPH::UVec4::sOr(invalid, NonFinite(correction));
        const auto delta = ToPose(correction);
        deltas.push_back({data.fk_bones[node.fk_index].sorted_index, 0, delta.position, delta.rotation});
    }
    if (invalid.TestAnyTrue()) [[unlikely]] {
        return Fail(StepOutcome::kNonFiniteState, peak);
    }
    has_stepped = stepped;
    sampled_last_step = stepped && capture != nullptr;
    sample_valid = true;
    return {.peak_bytes = peak};
}

std::span<const gfx::renderer::BonePhysicsDelta> PhysicsWorld::ActiveDeltas() const noexcept {
    return impl_->deltas;
}
}  // namespace ysm::physics
