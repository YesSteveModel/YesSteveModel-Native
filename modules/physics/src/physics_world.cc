#include "physics_world_internal.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include "native_joint.h"

namespace ysm::physics {
namespace {
using JPH::EActivation;

RigidPose ScaledFrame(RigidPose frame, float scale) {
    frame.position *= scale;
    return frame;
}

RigidPose AnimationFrame(const PhysicsNodeTarget& node, const RigidPose& frame) {
    return ComposePose(FromPose(node.pivot), ScaledFrame(frame, node.scale));
}

bool HasAnimationPose(const PhysicsNodeTarget& node, DeactivationKind reason) {
    return node.scale > 0 && reason != DeactivationKind::kInvalidPose && reason != DeactivationKind::kInactiveInput;
}

JPH::RefConst<JPH::Shape> ScaledShape(const Node& node, float scale, const JPH::Shape* anchor) {
    if (node.kind == schema::PhysicsNodeKind::kAnchor) {
        return anchor;
    }
    if (scale == 1) {
        return node.shape;
    }
    return new JPH::ScaledShape(node.shape, JPH::Vec3::sReplicate(scale));
}

void SetInertia(JPH::Body& body, const Node& node, float scale) {
    auto* motion = body.GetMotionProperties();
    motion->SetInverseMass(1.0f / node.mass);
    motion->SetInverseInertia(node.base_inv_inertia_diagonal / (scale * scale),
                              node.base_inertia_rotation);
}

// Jolt 5.7.0 uses one work set per substep in sequence, plus all four step
// records. Rigid discrete bodies create no CCD cast or soft body arrays.
size_t RigidStepScratchBytes(size_t bodies, size_t authored_joints, unsigned pairs, unsigned contacts) {
    constexpr size_t step_records = 4 * 16 * 1024;
    constexpr size_t alignment_and_sentinels = 64 * JPH_RVECTOR_ALIGNMENT;
    // Per contact: engine row, offset, component links and island indices.
    const size_t contact_rows = contacts * (JPH::ContactConstraintManager::cMaxConstraintSize + 8 * sizeof(uint32_t));
    // An authored joint may install both native and custom rows. Each needs
    // one active pointer, component link and island index.
    const size_t joints = 2 * authored_joints * (sizeof(void*) + 2 * sizeof(uint32_t));
    // Island starts, ends, sort scratch, active-to-CCD map and optional stats.
    const size_t islands = (bodies + 1) * 128;
    return step_records + alignment_and_sentinels + pairs * sizeof(JPH::BodyPair) + contact_rows + joints + islands;
}
}  // namespace

void ContactCapture::Begin(size_t count) {
    std::fill_n(reported_.begin(), count, Bitmap::value_type{});
    std::fill_n(touching_.begin(), count, Bitmap::value_type{});
}

void ContactCapture::Record(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m) {
    const auto i = a.GetCollisionGroup().GetSubGroupID();
    const auto j = b.GetCollisionGroup().GetSubGroupID();
    auto set = [i, j](Bitmap& bitmap) {
        bitmap[i][j / 64] |= uint64_t{1} << (j % 64);
        bitmap[j][i / 64] |= uint64_t{1} << (i % 64);
    };
    set(reported_);
    if (m.mPenetrationDepth >= 0) {
        set(touching_);
    }
}

void ContactCapture::OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                                    const JPH::ContactManifold& m, JPH::ContactSettings&) {
    Record(a, b, m);
}

void ContactCapture::OnContactPersisted(const JPH::Body& a, const JPH::Body& b,
                                        const JPH::ContactManifold& m, JPH::ContactSettings&) {
    Record(a, b, m);
}

ContactState ContactCapture::Read(size_t a, size_t b) const {
    const auto mask = uint64_t{1} << (b % 64);
    if ((touching_[a][b / 64] & mask) != 0) {
        return ContactState::kTouching;
    }
    return (reported_[a][b / 64] & mask) != 0 ? ContactState::kSpeculative : ContactState::kNoManifold;
}

PhysicsWorld::Impl::Impl(std::shared_ptr<const PhysicsAsset> owner)
    : asset(std::move(owner)), data(asset->Pimpl()), bodies(data.nodes.size()),
      joints(data.constraints.size()), targets(bodies.size()), reasons(bodies.size()),
      solved_pivots(data.dynamic_count), anchor_shape(new JPH::SphereShape(0.01f)) {
    deltas.reserve(data.dynamic_count);
    const auto n = static_cast<unsigned>(bodies.size());
    const auto pairs = std::clamp(n * n, 64u, 1024u);
    const auto contacts = std::clamp(n * 8, 64u, 512u);
    system.Init(std::max(16u, n), 1, pairs, contacts, layers, layers, layers);
    auto settings = system.GetPhysicsSettings();
    settings.mMaxInFlightBodyPairs = static_cast<int>(pairs);
    settings.mUseLargeIslandSplitter = false;  // A single worker cannot parallelize an island.
    settings.mPenetrationSlop = 0.004f;
    system.SetPhysicsSettings(settings);
    scratch_bytes = RigidStepScratchBytes(n, joints.size(), pairs, contacts);
    system.SetGravity(data.gravity);
    system.SetCombineFriction([](const JPH::Body& a, const JPH::SubShapeID&,
                                 const JPH::Body& b, const JPH::SubShapeID&) {
        return std::sqrt(a.GetFriction()) * std::sqrt(b.GetFriction());
    });
    // The engine's restitution combiner already takes the maximum.
}

PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;
PhysicsWorld::~PhysicsWorld() = default;

absl::StatusOr<PhysicsWorld> PhysicsWorld::Create(std::shared_ptr<const PhysicsAsset> asset) {
    try {
        return PhysicsWorld(std::make_unique<Impl>(std::move(asset)));
    } catch (const std::bad_alloc&) {
        return absl::ResourceExhaustedError("Physics world allocation failed.");
    }
}

void PhysicsWorld::Impl::RemoveJoint(size_t i) {
    auto& joint = joints[i];
    if (joint.native) {
        system.RemoveConstraint(joint.native);
    }
    if (joint.custom) {
        system.RemoveConstraint(joint.custom);
    }
    joint = {};
}

void PhysicsWorld::Impl::RemoveBody(size_t i) {
    for (const auto id : data.nodes[i].incident_constraints) {
        RemoveJoint(id);
    }
    auto& interface = system.GetBodyInterfaceNoLock();
    const auto id = bodies[i].body->GetID();
    interface.RemoveBody(id);
    interface.DestroyBody(id);
    bodies[i].body = nullptr;
}

void PhysicsWorld::Impl::CreateBody(size_t i, const RigidPose& pivot, float scale) {
    const auto& node = data.nodes[i];
    const bool dynamic = node.kind == schema::PhysicsNodeKind::kDynamic;
    const auto shape = ScaledShape(node, scale, anchor_shape);
    const auto pose = ComposePose(pivot, ScaledFrame(node.shape_from_bone, scale));
    JPH::BodyCreationSettings settings(shape, pose.position, pose.rotation,
        dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic,
        node.kind == schema::PhysicsNodeKind::kAnchor ? 0 : 1);
    settings.mCollisionGroup = {data.group_filter, node.collection_mask, static_cast<uint32_t>(i)};
    settings.mAllowSleeping = false;
    settings.mFriction = node.friction;
    settings.mRestitution = node.restitution;
    settings.mLinearDamping = node.linear_damping;
    settings.mAngularDamping = node.angular_damping;
    settings.mGravityFactor = node.gravity_factor;
    // Body creation needs a well-conditioned tensor even when the real inertia
    // is below Jolt's decomposition epsilon. Install the exact principal values afterwards.
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    settings.mMassPropertiesOverride.mMass = 1;
    settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
    auto& interface = system.GetBodyInterfaceNoLock();
    auto* body = interface.CreateBody(settings);
    if (dynamic) {
        SetInertia(*body, node, scale);
    }
    bodies[i] = {body, scale};
    interface.AddBody(body->GetID(), EActivation::Activate);
}

RigidPose PhysicsWorld::Impl::Pivot(size_t i) const {
    const auto& state = bodies[i];
    const auto& node = data.nodes[i];
    const auto rotation = state.body->GetRotation() * node.shape_from_bone.rotation.Conjugated();
    return {state.body->GetCenterOfMassPosition() - rotation * (state.scale * node.com_in_bone), rotation};
}

std::array<RigidPose, 2> PhysicsWorld::Impl::Frames(const Constraint& joint) const {
    std::array<RigidPose, 2> frames;
    for (size_t end = 0; end < 2; ++end) {
        const auto i = joint.nodes[end];
        const auto& n = data.nodes[i];
        const auto q = n.shape_from_bone.rotation.Conjugated();
        frames[end] = {q * ((joint.frames[end].position - n.com_in_bone) * bodies[i].scale),
                       q * joint.frames[end].rotation};
    }
    return frames;
}

void PhysicsWorld::Impl::RescaleBody(size_t i, float new_scale) {
    auto& state = bodies[i];
    const auto& node = data.nodes[i];
    const auto pivot = Pivot(i);
    auto& body = *state.body;
    auto& interface = system.GetBodyInterfaceNoLock();
    const auto shape = ScaledShape(node, new_scale, anchor_shape);
    interface.SetShape(body.GetID(), shape, false, EActivation::DontActivate);
    state.scale = new_scale;
    const auto com = pivot.position + pivot.rotation * (new_scale * node.com_in_bone);
    interface.SetPosition(body.GetID(), com - body.GetRotation() * shape->GetCenterOfMass(),
                          EActivation::DontActivate);
    if (body.IsDynamic()) {
        SetInertia(body, node, new_scale);
    }
    // SetShape(false), SetPosition and SetInverseInertia leave v and omega untouched.
    for (const auto id : node.incident_constraints) {
        auto& link = joints[id];
        if (!link.installed) {
            continue;
        }
        const auto& def = data.constraints[id];
        const auto frames = Frames(def);
        const auto a_scale = bodies[def.nodes[0]].scale;
        const size_t end = def.nodes[0] == i ? 0 : 1;
        if (link.native) {
            auto point = end == 0 ? NativeAttachmentA(def, frames[0], a_scale) : frames[1].position;
            link.native->NotifyShapeChanged(body.GetID(), link.native_points[end] - point);
            link.native_points[end] = point;
            if (end == 0) {
                RescaleNativeLimits(*link.native, def, a_scale);
            }
        }
        if (link.custom) {
            link.custom->SetFrames(frames, a_scale);
        }
    }
}

void PhysicsWorld::Impl::InstallJoint(size_t i) {
    const auto& def = data.constraints[i];
    if (!def.is_effective_mechanical) {
        return;
    }
    auto& a = bodies[def.nodes[0]];
    auto& b = bodies[def.nodes[1]];
    if (!a.body || !b.body) {
        return;
    }
    auto& link = joints[i];
    const auto frames = Frames(def);
    link.native = MakeNativeJoint(def, frames, a.scale, *a.body, *b.body);
    if (link.native) {
        link.native_points = {NativeAttachmentA(def, frames[0], a.scale), frames[1].position};
        system.AddConstraint(link.native);
    }
    if (SingleTrackJointConstraint::IsNeeded(def)) {
        link.custom = new SingleTrackJointConstraint(*a.body, *b.body, def, frames, a.scale);
        system.AddConstraint(link.custom);
        link.animation_target = std::ranges::any_of(def.axes, [](const Axis& axis) {
            return axis.target == schema::SpringTarget::kAnimation;
        });
    }
    JPH_ASSERT(link.native || link.custom);
    link.installed = true;
}

void PhysicsWorld::Impl::ApplyTargets(const PhysicsFrame& frame, float dt) {
    if (frame.nodes.data() != targets.data()) {
        // Pre-resolved targets are also a public input. Keep debug reads and the
        // next gathered topology coherent when callers alternate the two inputs.
        topology_known = false;
        std::ranges::copy(frame.nodes, targets.begin());
        for (size_t i = 0; i < targets.size(); ++i) {
            reasons[i] = targets[i].active ? DeactivationReason{} : DeactivationReason{
                DeactivationKind::kInactiveInput, data.fk_bones[data.nodes[i].fk_index].sorted_index};
        }
    }
    auto shift = RootOffset(frame.root_position, origin);
    auto& interface = system.GetBodyInterfaceNoLock();
    if (NeedsRebase(shift)) [[unlikely]] {
        for (const auto& state : bodies) {
            if (state.body) {
                interface.SetPosition(state.body->GetID(), state.body->GetPosition() - shift,
                                      EActivation::DontActivate);
            }
        }
        origin = frame.root_position;
        shift = JPH::Vec3::sZero();
    }
    root = {shift, Quaternion(frame.root_rotation)};
    for (size_t i = 0; i < bodies.size(); ++i) {
        auto& state = bodies[i];
        const auto& target = frame.nodes[i];
        if (!target.active) {
            if (state.body) {
                RemoveBody(i);
            }
            continue;
        }
        if (!state.body) {
            CreateBody(i, ComposePose(root, FromPose(target.pivot)), target.scale);
            continue;
        }
        if (std::abs(target.scale - state.scale) > 1e-4f * std::max(1.0f, state.scale)) [[unlikely]] {
            RescaleBody(i, target.scale);
        }
        if (state.body->IsKinematic()) {
            const auto target_shape = ComposePose(ComposePose(root, FromPose(target.pivot)),
                ScaledFrame(data.nodes[i].shape_from_bone, state.scale));
            interface.MoveKinematic(state.body->GetID(), target_shape.position, target_shape.rotation, dt);
        }
    }
    for (size_t i = 0; i < joints.size(); ++i) {
        auto& link = joints[i];
        const auto& def = data.constraints[i];
        if (!link.installed) {
            InstallJoint(i);
        }
        if (link.animation_target) {
            link.custom->SetAnimationTarget(RelativePose(
                AnimationFrame(frame.nodes[def.nodes[0]], def.frames[0]),
                AnimationFrame(frame.nodes[def.nodes[1]], def.frames[1])));
        }
    }
}

StepResult PhysicsWorld::Reset(const PhysicsFrame& frame) {
    auto& world = *impl_;
    for (size_t i = 0; i < world.bodies.size(); ++i) {
        if (world.bodies[i].body) {
            world.RemoveBody(i);
        }
    }
    world.origin = frame.root_position;
    world.has_stepped = false;
    world.sampled_last_step = false;
    try {
        world.ApplyTargets(frame, 0);
    } catch (const std::bad_alloc&) {
        return world.Fail(StepOutcome::kScratchExhausted);
    }
    return world.Publish(false, 0);
}

StepResult PhysicsWorld::Impl::Fail(StepOutcome outcome, size_t peak, uint32_t engine_error) {
    sample_valid = false;
    sampled_last_step = false;
    deltas.clear();
    return {.outcome = outcome, .engine_error = engine_error, .peak_bytes = peak};
}

int PhysicsWorld::Impl::CollisionSteps(float dt) {
    return std::clamp(static_cast<int>(std::ceil(dt / 0.05f - 1e-6f)), 1, 4);
}

StepResult PhysicsWorld::Impl::Finish(JPH::EPhysicsUpdateError error, size_t peak) {
    if (error != JPH::EPhysicsUpdateError::None) [[unlikely]] {
        return Fail(StepOutcome::kCapacityExceeded, peak, static_cast<uint32_t>(error));
    }
    auto invalid = JPH::UVec4::sZero();
    for (size_t i = 0; i < data.dynamic_count; ++i) {
        if (const auto* body = bodies[i].body) {
            const auto pose = NonFinite({body->GetCenterOfMassPosition(), body->GetRotation()});
            const auto velocity = JPH::UVec4::sOr(NonFinite(JPH::Vec4(body->GetLinearVelocity(), 0)),
                                                   NonFinite(JPH::Vec4(body->GetAngularVelocity(), 0)));
            invalid = JPH::UVec4::sOr(invalid, JPH::UVec4::sOr(pose, velocity));
        }
    }
    // Jolt's pivot/relative-pose operations require a unit quaternion even in debug builds.
    if (invalid.TestAnyTrue()) [[unlikely]] {
        return Fail(StepOutcome::kNonFiniteState, peak);
    }
    return Publish(true, peak);
}

StepResult PhysicsWorld::Step(const PhysicsFrame& frame, float dt) {
    return impl_->Advance(frame, dt, WorkerScratch::Current);
}

bool PhysicsWorld::HasSample() const { return impl_->sample_valid; }
bool PhysicsWorld::IsNodeActive(size_t slot) const { return impl_->bodies[slot].body != nullptr; }

PhysicsPose PhysicsWorld::ReadNodePivot(size_t slot) const {
    return ToPose(RelativePose(impl_->root, impl_->Pivot(slot)));
}

const PhysicsNodeTarget& PhysicsWorld::ReadAnimationTarget(size_t slot) const {
    return impl_->targets[slot];
}

std::optional<PhysicsPose> PhysicsWorld::ReadJointAnimationTarget(size_t joint) const {
    const auto& def = impl_->data.constraints[joint];
    for (const auto node : def.nodes) {
        if (!HasAnimationPose(impl_->targets[node], impl_->reasons[node].kind)) {
            return std::nullopt;
        }
    }
    return ToPose(RelativePose(AnimationFrame(impl_->targets[def.nodes[0]], def.frames[0]),
                               AnimationFrame(impl_->targets[def.nodes[1]], def.frames[1])));
}

DeactivationReason PhysicsWorld::ReadDeactivationReason(size_t slot) const {
    return impl_->reasons[slot];
}

void PhysicsWorld::SetDebugContactCapture(bool enabled) {
    if (enabled == (impl_->capture != nullptr)) {
        return;
    }
    if (enabled) {
        impl_->capture = std::make_unique<ContactCapture>();
        impl_->system.SetContactListener(impl_->capture.get());
    } else {
        impl_->system.SetContactListener(nullptr);
        impl_->capture.reset();
    }
    impl_->sampled_last_step = false;
}

ContactState PhysicsWorld::ReadContact(size_t a, size_t b) const {
    if (!HasSample()) {
        return ContactState::kNoSample;
    }
    const auto* first = impl_->bodies[a].body;
    const auto* second = impl_->bodies[b].body;
    if (a == b || !first || !second || (!first->IsDynamic() && !second->IsDynamic()) ||
        !first->GetCollisionGroup().CanCollide(second->GetCollisionGroup())) {
        return ContactState::kIneligible;
    }
    if (!impl_->has_stepped) {
        return ContactState::kNoStep;
    }
    return impl_->sampled_last_step ? impl_->capture->Read(a, b) : ContactState::kNotSampled;
}

bool PhysicsWorld::IsRotationSingular(size_t joint) const {
    const auto& state = impl_->joints[joint];
    if (state.custom) {
        return state.custom->IsRotationSingular();
    }
    const auto& def = impl_->data.constraints[joint];
    const auto* first = impl_->bodies[def.nodes[0]].body;
    const auto* second = impl_->bodies[def.nodes[1]].body;
    if (!first || !second) {
        return false;
    }
    const auto frames = impl_->Frames(def);
    const auto a = first->GetRotation() * frames[0].rotation;
    const auto b = second->GetRotation() * frames[1].rotation;
    return SingleTrackJointConstraint::IsRotationSingular(a.Conjugated() * b, def.kind);
}

DebugSnapshotOut::Layout PhysicsWorld::DebugSnapshotLayout() const {
    const auto nodes = impl_->bodies.size();
    const auto joints = impl_->joints.size();
    const auto pairs = (nodes * nodes - nodes) / 2;
    return {nodes * DebugSnapshotOut::kNodeFloats + joints * DebugSnapshotOut::kJointFloats,
            nodes * DebugSnapshotOut::kNodeInts + joints + pairs};
}

int32_t PhysicsWorld::ReadDebugSnapshot(DebugSnapshotOut output) const {
    if (!HasSample()) {
        return 0;
    }

    constexpr int32_t kPoseAvailable = 1, kActive = 2;
    constexpr int32_t kFrameA = 1, kFrameB = 2, kCurrent = 4, kAnimation = 8;
    constexpr int32_t kCurrentSingular = 16, kRestSingular = 32, kAnimationSingular = 64;
    const auto write_pose = [](std::span<float> destination, const PhysicsPose& pose) {
        std::ranges::copy(pose.position, destination.begin());
        std::ranges::copy(pose.rotation, destination.begin() + 3);
    };
    const auto write_reading = [](std::span<float> destination, const RigidPose& pose,
                                  schema::PhysicsConstraintKind kind) {
        const auto [angles, singular] = SingleTrackJointConstraint::ReadDebugAngles(pose.rotation, kind);
        for (size_t i = 0; i < 3; ++i) {
            destination[i] = pose.position[i];
            destination[i + 3] = angles[i];
        }
        return singular;
    };

    std::ranges::fill(output.floats, 0.0f);
    const auto nodes = impl_->bodies.size();
    const auto joints = impl_->joints.size();
    std::array<PhysicsNodeTarget, kMaxNodes> model_pivots;
    std::array<bool, kMaxNodes> pose_available;
    std::array<uint8_t, kMaxNodes> slot_by_author;
    for (size_t i = 0; i < nodes; ++i) {
        const auto& body = impl_->bodies[i];
        const auto& target = impl_->targets[i];
        const auto reason = ReadDeactivationReason(i);
        const auto author = impl_->data.nodes[i].authored_index;
        const bool active = body.body != nullptr;
        pose_available[i] = active || HasAnimationPose(target, reason.kind);
        model_pivots[i] = {active ? ReadNodePivot(i) : target.pivot,
                           active ? body.scale : target.scale, active};
        slot_by_author[author] = static_cast<uint8_t>(i);

        auto floats = output.floats.subspan(author * DebugSnapshotOut::kNodeFloats, DebugSnapshotOut::kNodeFloats);
        if (pose_available[i]) {
            write_pose(floats, model_pivots[i].pivot);
        }
        floats[7] = model_pivots[i].scale;
        auto ints = output.ints.subspan(author * DebugSnapshotOut::kNodeInts, DebugSnapshotOut::kNodeInts);
        ints[0] = (pose_available[i] ? kPoseAvailable : 0) | (active ? kActive : 0);
        ints[1] = static_cast<int32_t>(reason.kind);
        ints[2] = reason.culprit;
    }

    const size_t joint_floats = nodes * DebugSnapshotOut::kNodeFloats;
    const size_t joint_ints = nodes * DebugSnapshotOut::kNodeInts;
    for (size_t i = 0; i < joints; ++i) {
        const auto& joint = impl_->data.constraints[i];
        const auto [a, b] = joint.nodes;
        auto floats = output.floats.subspan(joint_floats + i * DebugSnapshotOut::kJointFloats,
                                           DebugSnapshotOut::kJointFloats);
        int32_t flags = 0;
        std::array<RigidPose, 2> frames;
        for (size_t end = 0; end < 2; ++end) {
            const auto node = joint.nodes[end];
            if (pose_available[node]) {
                // Keep the authored attachment for readout, even when the native row
                // folds a nonzero equality offset into its internal attachment.
                frames[end] = AnimationFrame(model_pivots[node], joint.frames[end]);
                write_pose(floats.subspan(end * 7), ToPose(frames[end]));
                flags |= end == 0 ? kFrameA : kFrameB;
            }
        }
        floats[14] = model_pivots[a].scale;
        if (model_pivots[a].active && model_pivots[b].active) {
            flags |= kCurrent;
            if (write_reading(floats.subspan(15), RelativePose(frames[0], frames[1]), joint.kind)) {
                flags |= kCurrentSingular;
            }
        }
        if (write_reading(floats.subspan(21), joint.rest_target, joint.kind)) {
            flags |= kRestSingular;
        }
        if (const auto animation = ReadJointAnimationTarget(i)) {
            flags |= kAnimation;
            if (write_reading(floats.subspan(27), FromPose(*animation), joint.kind)) {
                flags |= kAnimationSingular;
            }
        }
        output.ints[joint_ints + i] = flags;
    }

    auto contacts = output.ints.subspan(joint_ints + joints);
    size_t pair = 0;
    for (size_t high = 1; high < nodes; ++high) {
        for (size_t low = 0; low < high; ++low) {
            contacts[pair++] = static_cast<int32_t>(ReadContact(slot_by_author[high], slot_by_author[low]));
        }
    }
    return DebugSnapshotOut::kSample | (impl_->has_stepped ? DebugSnapshotOut::kStepped : 0) |
           (impl_->sampled_last_step ? DebugSnapshotOut::kContactSampled : 0);
}

}  // namespace ysm::physics
