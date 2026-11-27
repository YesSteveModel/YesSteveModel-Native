#pragma once

#include "physics/physics_world.h"

#include <bitset>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "gfx/renderer/physics_delta.h"
#include "single_track_joint_constraint.h"
#include "worker_scratch.h"

namespace ysm::physics {

inline JPH::Quat Quaternion(const std::array<float, 4>& q) {
    return {q[0], q[1], q[2], q[3]};
}

inline RigidPose FromPose(const PhysicsPose& pose) {
    return {{pose.position[0], pose.position[1], pose.position[2]}, Quaternion(pose.rotation)};
}

inline PhysicsPose ToPose(const RigidPose& pose) {
    const auto& p = pose.position;
    const auto& q = pose.rotation;
    return {{p.GetX(), p.GetY(), p.GetZ()}, {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
}

inline JPH::UVec4 NonFinite(JPH::Vec4Arg value) {
    const auto exponent = JPH::UVec4::sReplicate(0x7f800000);
    return JPH::UVec4::sEquals(JPH::UVec4::sAnd(value.ReinterpretAsInt(), exponent), exponent);
}

inline JPH::UVec4 NonFinite(const RigidPose& pose) {
    return JPH::UVec4::sOr(NonFinite(JPH::Vec4(pose.position, 0)), NonFinite(pose.rotation.GetXYZW()));
}

inline JPH::Vec3 RootOffset(const std::array<double, 3>& position,
                            const std::array<double, 3>& origin) {
    return {static_cast<float>(position[0] - origin[0]), static_cast<float>(position[1] - origin[1]),
            static_cast<float>(position[2] - origin[2])};
}

inline bool NeedsRebase(JPH::Vec3Arg offset) { return offset.LengthSq() > 64; }

class WorldLayers final : public JPH::BroadPhaseLayerInterface,
                          public JPH::ObjectVsBroadPhaseLayerFilter,
                          public JPH::ObjectLayerPairFilter {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {
        return JPH::BroadPhaseLayer(0);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Model"; }
#endif
    // Anchor bodies carry a Jolt shape only to provide a moving constraint endpoint.
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer) const override { return a != 0; }
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a != 0 && b != 0;
    }
};

class ContactCapture final : public JPH::ContactListener {
public:
    void Begin(size_t count);
    ContactState Read(size_t a, size_t b) const;
    void OnContactAdded(const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&,
                        JPH::ContactSettings&) override;
    void OnContactPersisted(const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&,
                            JPH::ContactSettings&) override;

private:
    void Record(const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&);
    using Bitmap = std::array<std::array<uint64_t, 2>, kMaxNodes>;
    Bitmap reported_{};
    Bitmap touching_{};
};

class PhysicsWorld::Impl {
public:
    explicit Impl(std::shared_ptr<const PhysicsAsset> owner);
    struct BodyState {
        // Jolt owns the body until RemoveBody destroys its ID.
        JPH::Body* body = nullptr;
        float scale = 1;
    };
    struct JointState {
        JPH::Ref<JPH::TwoBodyConstraint> native;
        JPH::Ref<SingleTrackJointConstraint> custom;
        std::array<JPH::Vec3, 2> native_points{JPH::Vec3::sZero(), JPH::Vec3::sZero()};
        bool installed = false;
        bool animation_target = false;
    };

    void RemoveJoint(size_t i);
    void RemoveBody(size_t i);
    void CreateBody(size_t i, const RigidPose& pivot, float scale);
    void RescaleBody(size_t i, float new_scale);
    RigidPose Pivot(size_t i) const;
    std::array<RigidPose, 2> Frames(const Constraint& joint) const;
    void InstallJoint(size_t i);
    void EvaluateBones(WorkerScratch& scratch, const RigidPose& sim_root, bool valid_root);
    void UpdateAnchoring(const std::bitset<kMaxNodes>& eligible);
    void ApplyTargets(const PhysicsFrame& frame, float dt);
    StepResult Publish(bool stepped, size_t peak);
    StepResult Finish(JPH::EPhysicsUpdateError error, size_t peak);
    StepResult Fail(StepOutcome outcome, size_t peak = 0, uint32_t engine_error = 0);
    // Scratch acquisition and growth must finish before entering Jolt's locked update.
    template<class Acquire>
    StepResult Advance(const PhysicsFrame& frame, float dt, Acquire&& acquire) {
        WorkerScratch* scratch;
        try {
            scratch = &acquire();
            ApplyTargets(frame, dt);
        } catch (const std::bad_alloc&) {
            return Fail(StepOutcome::kScratchExhausted);
        }
        if (!scratch->temp.BeginStep(scratch_bytes)) [[unlikely]] {
            return Fail(StepOutcome::kScratchExhausted);
        }
        if (capture) {
            capture->Begin(bodies.size());
        }
        const auto error = system.Update(dt, CollisionSteps(dt), &scratch->temp, &scratch->jobs);
        scratch->temp.EndStep();
        return Finish(error, scratch->temp.PeakBytes());
    }
    static int CollisionSteps(float dt);

    std::shared_ptr<const PhysicsAsset> asset;
    const PhysicsAsset::Impl& data;
    WorldLayers layers;
    std::unique_ptr<ContactCapture> capture;
    JPH::PhysicsSystem system;
    std::vector<BodyState> bodies;
    std::vector<JointState> joints;
    std::vector<PhysicsNodeTarget> targets;
    std::vector<DeactivationReason> reasons;
    std::vector<RigidPose> solved_pivots;
    std::vector<gfx::renderer::BonePhysicsDelta> deltas;
    std::bitset<kMaxNodes> direct_eligible;
    bool topology_known = false;
    JPH::RefConst<JPH::Shape> anchor_shape;
    size_t scratch_bytes;
    std::array<double, 3> origin{};
    RigidPose root;
    bool sample_valid = false;
    bool has_stepped = false;
    bool sampled_last_step = false;
};

}  // namespace ysm::physics
