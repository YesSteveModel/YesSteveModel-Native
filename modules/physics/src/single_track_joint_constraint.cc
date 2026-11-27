#include "single_track_joint_constraint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <variant>

#include <Jolt/Physics/Constraints/ConstraintPart/AngleConstraintPart.h>
#include <Jolt/Physics/Constraints/ConstraintPart/AxisConstraintPart.h>

namespace ysm::physics {
namespace {
using JPH::Body;
using JPH::Quat;
using JPH::Vec3;
using Kind = schema::PhysicsConstraintKind;
using Limit = schema::LimitKind;
using Target = schema::SpringTarget;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr unsigned kCone = 6;
constexpr unsigned kAlignedZ = 7;
constexpr float kSingularTwistNormSq = 1.0e-8f;

enum class Role { kMotor, kSoft, kHard };

Vec3 Basis(unsigned i) {
    return i == 0 ? Vec3::sAxisX() : i == 1 ? Vec3::sAxisY() : Vec3::sAxisZ();
}

float Wrap(float angle) {
    if (angle > kPi) {
        angle -= 2 * kPi;
    } else if (angle <= -kPi) {
        angle += 2 * kPi;
    }
    // The cosine of a stored float half-turn can round to either side of zero.
    constexpr float half_turn_ulp = 2 * std::numeric_limits<float>::epsilon();
    return std::abs(std::abs(angle) - kPi) <= half_turn_ulp ? kPi : angle;
}

bool Periodic(Kind kind, unsigned slot) {
    return slot == (kind == Kind::kHinge ? 5u : 3u);
}

bool FullCircle(const Axis& axis) {
    return axis.lower <= -kPi && axis.upper >= kPi;
}

float Unwrap(float angle, const Axis& axis) {
    const float midpoint = 0.5f * (axis.lower + axis.upper);
    return midpoint + Wrap(angle - midpoint);
}

template <class F>
void VisitRows(const Constraint& definition, F&& visit) {
    if (definition.kind == Kind::kFixed) {
        return;
    }
    const bool native_rotation = definition.cone.limit == Limit::kFree &&
        std::all_of(definition.axes.begin() + 3, definition.axes.end(),
                    [](const Axis& axis) { return axis.limit == Limit::kLocked; });
    for (const auto role : {Role::kMotor, Role::kSoft, Role::kHard}) {
        for (unsigned slot = 0; slot < definition.axes.size(); ++slot) {
            if ((slot < 3 && definition.kind != Kind::kGeneric) ||
                (definition.kind == Kind::kHinge && slot != 5)) {
                continue;
            }
            const auto& axis = definition.axes[slot];
            // Both twist and each component of the swing rotation vector are bounded by pi.
            const bool whole = slot >= 3 && FullCircle(axis);
            if (role == Role::kMotor && axis.target != Target::kOff && axis.target_spring.HasStiffnessOrDamping()) {
                visit(slot, role);
            } else if (role == Role::kSoft && axis.limit == Limit::kSoft && !whole && axis.soft_spring.HasStiffnessOrDamping()) {
                visit(slot, role);
            } else if (role == Role::kHard && slot >= 3 && !native_rotation && definition.kind != Kind::kHinge &&
                       (axis.limit == Limit::kLocked || (axis.limit == Limit::kHard && !whole))) {
                visit(slot, role);
            }
        }
        const auto& cone = definition.cone;
        if (cone.upper < kPi &&
            ((role == Role::kHard && cone.limit == Limit::kHard) ||
             (role == Role::kSoft && cone.limit == Limit::kSoft && cone.soft_spring.HasStiffnessOrDamping()))) {
            visit(kCone, role);
            if (role == Role::kHard && cone.upper == 0) {
                visit(kAlignedZ, role);
            }
        }
    }
}

struct RotationRead {
    Vec3 angles = Vec3::sZero();
    std::array<Vec3, 3> gradients{Vec3::sZero(), Vec3::sZero(), Vec3::sZero()};
    bool singular = false;
};

template <bool Gradients>
RotationRead ReadRotation(JPH::QuatArg q, bool hinge) {
    RotationRead result;
    const float w = q.GetW(), x = q.GetX(), y = q.GetY(), z = q.GetZ();
    const float twist_component = hinge ? z : x;
    const float h2 = w * w + twist_component * twist_component;
    if (h2 < kSingularTwistNormSq) {
        result.singular = true;
        return result;
    }
    const float sign = w < 0 ? -1.0f : 1.0f;
    const float twist = Wrap(2.0f * std::atan2(sign * twist_component, sign * w));
    if (hinge) {
        result.angles = Vec3(0, 0, twist);
        if constexpr (Gradients) {
            result.gradients[2] = Vec3(w * y + z * x, z * y - w * x, h2) / h2;
        }
        return result;
    }

    const float h = std::sqrt(h2);
    const float sy = (w * y - x * z) / h;
    const float sz = (w * z + x * y) / h;
    const float sine_half = std::sqrt(sy * sy + sz * sz);
    const float angle = 2.0f * std::atan2(sine_half, h);
    const float scale = sine_half > 1.0e-6f ? angle / sine_half : 2.0f;
    result.angles = Vec3(twist, sy * scale, sz * scale);
    if constexpr (Gradients) {
        result.gradients[0] = Vec3(1, sz / h, -sy / h);
        if (sine_half <= 1.0e-6f) {
            result.gradients[1] = Vec3(-sz * 2, 1, 0);
            result.gradients[2] = Vec3(sy * 2, 0, 1);
        } else {
            // B's X direction is cos(angle) X + sin(angle) tangent in A.
            // Differentiating its polar angle and azimuth gives both swing-log rows.
            const Vec3 axis(0, sy / sine_half, sz / sine_half);
            const Vec3 tangent(0, axis.GetZ(), -axis.GetY());
            const float angle_cotangent = angle * (h2 - sine_half * sine_half) / (2 * h * sine_half);
            const Vec3 azimuth = angle_cotangent * tangent - angle * Vec3::sAxisX();
            result.gradients[1] = axis.GetY() * axis + tangent.GetY() * azimuth;
            result.gradients[2] = axis.GetZ() * axis + tangent.GetZ() * azimuth;
        }
    }
    return result;
}

struct Coordinate {
    float value = 0;
    Vec3 gradient = Vec3::sZero();
};

std::pair<float, float> Bounds(const Constraint& definition, unsigned slot, float scale) {
    if (slot >= kCone) {
        if (definition.cone.limit == Limit::kHard && definition.cone.upper == 0) {
            return {0, 0};
        }
        return {-kInfinity, definition.cone.upper};
    }
    const Axis& axis = definition.axes[slot];
    if (axis.limit == Limit::kLocked) {
        return {0, 0};
    }
    const float units = slot < 3 ? scale : 1;
    return {axis.lower * units, axis.upper * units};
}

class JointSettings final : public JPH::TwoBodyConstraintSettings {
public:
    JointSettings(const Constraint& definition, const std::array<RigidPose, 2>& frames, float scale)
        : definition_(definition), frames_(frames), scale_(scale) {}
    JPH::TwoBodyConstraint* Create(Body& a, Body& b) const override {
        auto* joint = new SingleTrackJointConstraint(a, b, definition_, frames_, scale_);
        joint->SetEnabled(mEnabled);
        joint->SetConstraintPriority(mConstraintPriority);
        joint->SetNumVelocityStepsOverride(mNumVelocityStepsOverride);
        joint->SetNumPositionStepsOverride(mNumPositionStepsOverride);
        joint->SetUserData(mUserData);
        return joint;
    }
private:
    const Constraint& definition_;
    std::array<RigidPose, 2> frames_;
    float scale_;
};

}  // namespace

struct SingleTrackJointConstraint::Geometry {
    Quat frame_a;
    Vec3 lever_a = Vec3::sZero();
    Vec3 lever_b = Vec3::sZero();
    std::array<Coordinate, 8> coordinates;
};

struct SingleTrackJointConstraint::Row {
    struct ImpulseLaw {
        float bias;
        float velocity_gain;
        float At(float free_rate) const { return -(bias + velocity_gain * free_rate); }
    };

    Row(unsigned coordinate, Role purpose) : slot(coordinate), role(purpose) {
        if (slot >= 3) {
            part.emplace<JPH::AngleConstraintPart>();
        }
    }

    void Deactivate() {
        std::visit([](auto& p) { p.Deactivate(); }, part);
        inverse_mass = 0;
        side = 0;
    }

    void PrepareGeometry(Body& a, Body& b, const Geometry& geometry, Vec3 direction) {
        normal = direction;
        const bool linear = slot < 3;
        auto body_mass = [&](const Body& body, Vec3 lever) {
            if (!body.IsDynamic()) {
                return 0.0f;
            }
            const Vec3 moment = linear ? lever.Cross(normal) : normal;
            const auto* motion = body.GetMotionProperties();
            const Vec3 response = motion->MultiplyWorldSpaceInverseInertiaByVector(body.GetRotation(), moment);
            return moment.Dot(response) + (linear ? motion->GetInverseMass() : 0.0f);
        };
        inverse_mass = body_mass(a, geometry.lever_a) + body_mass(b, geometry.lever_b);
        if (!(inverse_mass >= std::numeric_limits<float>::min())) {
            Deactivate();
        } else if (linear) {
            std::get<JPH::AxisConstraintPart>(part).CalculateConstraintProperties(a, geometry.lever_a, b, geometry.lever_b, normal);
        } else {
            std::get<JPH::AngleConstraintPart>(part).CalculateConstraintProperties(a, b, normal);
        }
    }

    void PrepareSpring(const JPH::SpringSettings& spring, float dt, float error, float length) {
        double k, c;
        if (spring.mMode == JPH::ESpringMode::FrequencyAndDamping) {
            const double omega = 2.0 * std::numbers::pi * spring.mFrequency;
            k = omega * omega / inverse_mass;
            c = 2.0 * spring.mDamping * omega / inverse_mass;
        } else {
            // The public parts take unit axes; virtual work scales both physical coefficients by |J|².
            const double squared = static_cast<double>(length) * length;
            k = spring.mStiffness * squared;
            c = spring.mDamping * squared;
        }
        auto law = [&](double damping) -> ImpulseLaw {
            const double velocity_term = dt * (dt * k + damping);
            const double denominator = 1.0 + inverse_mass * velocity_term;
            return {static_cast<float>(dt * k * error / denominator),
                    static_cast<float>(velocity_term / denominator)};
        };
        damped = law(c);
        if (role == Role::kSoft) {
            elastic = law(0);
        }
    }

    bool Solve(Body& a, Body& b, Vec3 lever_a, Vec3 lever_b) {
        if (inverse_mass == 0) {
            return false;
        }
        const Vec3 relative_velocity = slot < 3 ? b.GetPointVelocityCOM(lever_b) - a.GetPointVelocityCOM(lever_a)
                                               : b.GetAngularVelocity() - a.GetAngularVelocity();
        return std::visit([&](auto& p) {
            // Remove only this row's accumulated impulse. Other motors, geometry and contacts stay coupled.
            const float free_rate = normal.Dot(relative_velocity) - inverse_mass * p.GetTotalLambda();
            float impulse;
            if (role == Role::kHard) {
                impulse = (std::clamp(free_rate, lower_rate, upper_rate) - free_rate) / inverse_mass;
            } else if (role == Role::kMotor) {
                impulse = damped.At(free_rate);
            } else {
                // The more inward of these implicit solutions applies damping exactly when the
                // predicted velocity points out of the band. The final clamp prevents an outward pull.
                const float spring_only = elastic.At(free_rate);
                const float spring_damper = damped.At(free_rate);
                impulse = side > 0 ? std::min({0.0f, spring_only, spring_damper})
                                   : std::max({0.0f, spring_only, spring_damper});
            }
            // Equal impulse bounds apply the prescribed total through Jolt's paired body Jacobian.
            return p.SolveVelocityConstraint(a, b, normal, impulse, impulse);
        }, part);
    }

    unsigned slot;
    Role role;
    std::variant<JPH::AxisConstraintPart, JPH::AngleConstraintPart> part;
    Vec3 normal = Vec3::sZero();
    float inverse_mass = 0;
    float lower_rate = 0;
    float upper_rate = 0;
    ImpulseLaw elastic{}, damped{};
    int side = 0;
};

bool SingleTrackJointConstraint::IsNeeded(const ysm::physics::Constraint& definition) {
    bool found = false;
    VisitRows(definition, [&](unsigned, Role) { found = true; });
    return found;
}

SingleTrackJointConstraint::SingleTrackJointConstraint(Body& a, Body& b, const ysm::physics::Constraint& definition,
                                                       const std::array<RigidPose, 2>& frames_com, float a_scale)
    : TwoBodyConstraint(a, b, JointSettings(definition, frames_com, a_scale)),
      definition_(definition), frames_(frames_com), a_scale_(a_scale) {
    bool has_rest_rotation = false;
    size_t count = 0;
    VisitRows(definition, [&](unsigned, Role) { ++count; });
    rows_.reserve(count);
    VisitRows(definition, [&](unsigned slot, Role role) {
        rows_.emplace_back(slot, role);
        has_linear_ |= slot < 3;
        has_cone_ |= slot >= kCone;
        if (slot >= 3 && slot < kCone) {
            has_rotation_axes_ = true;
            if (role == Role::kMotor) {
                has_rest_rotation |= definition.axes[slot].target == Target::kRestPose;
                has_animation_rotation_ |= definition.axes[slot].target == Target::kAnimation;
            }
        }
        first_hard_ += role != Role::kHard;
    });
    if (has_rest_rotation) {
        const auto read = ReadRotation<false>(definition.rest_target.rotation, definition.kind == Kind::kHinge);
        rest_angles_ = read.angles;
        rest_singular_ = read.singular;
    }
    SetAnimationTarget({definition.rest_target.position * a_scale, definition.rest_target.rotation});
}

SingleTrackJointConstraint::~SingleTrackJointConstraint() = default;

void SingleTrackJointConstraint::SetFrames(const std::array<RigidPose, 2>& frames_com, float a_scale) {
    frames_ = frames_com;
    a_scale_ = a_scale;
    ResetWarmStart();
}

void SingleTrackJointConstraint::SetAnimationTarget(const RigidPose& relative) {
    animation_position_ = relative.position;
    if (has_animation_rotation_) {
        const auto read = ReadRotation<false>(relative.rotation, definition_.kind == Kind::kHinge);
        animation_angles_ = read.angles;
        animation_singular_ = read.singular;
    }
}

std::pair<JPH::Vec3, bool> SingleTrackJointConstraint::ReadDebugAngles(JPH::QuatArg relative, Kind kind) {
    const auto read = ReadRotation<false>(relative, kind == Kind::kHinge);
    return {read.angles, read.singular};
}

bool SingleTrackJointConstraint::IsRotationSingular(JPH::QuatArg q, schema::PhysicsConstraintKind kind) {
    const float t = kind == Kind::kHinge ? q.GetZ() : q.GetX();
    return q.GetW() * q.GetW() + t * t < kSingularTwistNormSq;
}

bool SingleTrackJointConstraint::IsRotationSingular() const {
    const Quat a = mBody1->GetRotation() * frames_[0].rotation;
    const Quat b = mBody2->GetRotation() * frames_[1].rotation;
    return IsRotationSingular(a.Conjugated() * b, definition_.kind) || rest_singular_ || animation_singular_;
}

JPH::Mat44 SingleTrackJointConstraint::GetConstraintToBody1Matrix() const {
    return JPH::Mat44::sRotationTranslation(frames_[0].rotation, frames_[0].position);
}

JPH::Mat44 SingleTrackJointConstraint::GetConstraintToBody2Matrix() const {
    return JPH::Mat44::sRotationTranslation(frames_[1].rotation, frames_[1].position);
}

JPH::Ref<JPH::ConstraintSettings> SingleTrackJointConstraint::GetConstraintSettings() const {
    JPH::Ref<JPH::ConstraintSettings> settings = new JointSettings(definition_, frames_, a_scale_);
    ToConstraintSettings(*settings);
    return settings;
}

void SingleTrackJointConstraint::NotifyShapeChanged(const JPH::BodyID& body, JPH::Vec3Arg delta_com) {
    if (mBody1->GetID() == body) {
        frames_[0].position -= delta_com;
    }
    if (mBody2->GetID() == body) {
        frames_[1].position -= delta_com;
    }
    ResetWarmStart();
}

SingleTrackJointConstraint::Geometry SingleTrackJointConstraint::Measure(bool include_linear) const {
    Geometry state;
    state.frame_a = mBody1->GetRotation() * frames_[0].rotation;
    if (include_linear) {
        state.lever_b = mBody2->GetRotation() * frames_[1].position;
        // A's axes rotate as well as its attachment: the A-side velocity is evaluated at B's attachment.
        state.lever_a = Vec3(mBody2->GetCenterOfMassPosition() - mBody1->GetCenterOfMassPosition()) + state.lever_b;
        const Vec3 offset = state.frame_a.Conjugated() * (state.lever_a - mBody1->GetRotation() * frames_[0].position);
        for (unsigned i = 0; i < 3; ++i) {
            state.coordinates[i] = {offset[i], Basis(i)};
        }
    }
    if (!has_rotation_axes_ && !has_cone_) {
        return state;
    }
    const Quat relative = state.frame_a.Conjugated() * (mBody2->GetRotation() * frames_[1].rotation);
    if (has_rotation_axes_) {
        const auto read = ReadRotation<true>(relative, definition_.kind == Kind::kHinge);
        for (unsigned i = 0; i < 3; ++i) {
            state.coordinates[3 + i] = {read.angles[i], read.gradients[i]};
        }
    }
    if (has_cone_) {
        const Vec3 x_b = relative.RotateAxisX();
        const Vec3 cross = Vec3::sAxisX().Cross(x_b);
        const float sine = cross.Length();
        const float angle = std::atan2(sine, x_b.GetX());
        // The antipode has no azimuth. Choosing A's Y makes recovery rotate with the rig.
        const bool antipode = x_b.GetX() < 0 && sine < 1.0e-5f;
        Vec3 normal = Vec3::sZero();
        if (antipode) {
            if (definition_.cone.limit == Limit::kHard || definition_.cone.soft_spring.HasStiffness()) {
                normal = Vec3::sAxisY();
            }
        } else if (sine > 0) {
            normal = cross / sine;
        }
        if (definition_.cone.limit == Limit::kHard && definition_.cone.upper == 0) {
            state.coordinates[kCone] = {angle * normal.GetY(), Vec3::sAxisY()};
            state.coordinates[kAlignedZ] = {angle * normal.GetZ(), Vec3::sAxisZ()};
        } else {
            state.coordinates[kCone] = {angle, normal};
        }
    }
    return state;
}

void SingleTrackJointConstraint::ResetWarmStart() {
    for (auto& row : rows_) {
        row.Deactivate();
    }
}

void SingleTrackJointConstraint::SetupVelocityConstraint(float dt) {
    const auto state = Measure(has_linear_);
    lever_a_ = state.lever_a;
    lever_b_ = state.lever_b;
    for (auto& row : rows_) {
        const unsigned slot = row.slot;
        const auto& coordinate = state.coordinates[slot];
        const auto& axis = slot >= kCone ? definition_.cone : definition_.axes[slot];
        const bool linear = slot < 3;
        const bool animation = axis.target == Target::kAnimation;
        const bool target_singular = animation ? animation_singular_ : rest_singular_;
        const float length = coordinate.gradient.Length();
        if (length == 0 || (row.role == Role::kMotor && !linear && target_singular)) {
            row.Deactivate();
            continue;
        }
        const bool periodic = Periodic(definition_.kind, slot);
        const bool bounded_hard = axis.limit == Limit::kHard && !FullCircle(axis);
        float value = coordinate.value;
        if (periodic && (bounded_hard || (axis.limit == Limit::kSoft && row.role != Role::kMotor))) {
            value = Unwrap(value, axis);
        }
        float error = 0;
        if (row.role == Role::kMotor) {
            const Vec3 target_vector = linear ? (animation ? animation_position_ : definition_.rest_target.position * a_scale_)
                                             : (animation ? animation_angles_ : rest_angles_);
            const float target = target_vector[slot % 3];
            error = value - target;
            if (periodic) {
                error = bounded_hard ? value - Unwrap(target, axis) : -Wrap(target - value);
            }
        } else {
            const auto [lo, hi] = Bounds(definition_, slot, a_scale_);
            error = value - std::clamp(value, lo, hi);
            if (row.role == Role::kSoft) {
                if (error == 0) {
                    row.Deactivate();
                    continue;
                }
                const int side = error > 0 ? 1 : -1;
                if (row.side != side) {
                    row.Deactivate();
                    row.side = side;
                }
            } else if (lo == hi) {
                row.lower_rate = row.upper_rate = 0;
            } else {
                // Speculate only as far as the actual stop. Split position correction removes existing penetration.
                row.lower_rate = std::min(0.0f, lo - value) / (length * dt);
                row.upper_rate = std::max(0.0f, hi - value) / (length * dt);
            }
        }
        row.PrepareGeometry(*mBody1, *mBody2, state, state.frame_a * (coordinate.gradient / length));
        if (row.role != Role::kHard && row.inverse_mass != 0) {
            row.PrepareSpring(row.role == Role::kMotor ? axis.target_spring : axis.soft_spring, dt, error / length, length);
        }
    }
}

void SingleTrackJointConstraint::WarmStartVelocityConstraint(float ratio) {
    for (auto& row : rows_) {
        if (row.inverse_mass == 0) {
            continue;
        }
        if (auto* linear = std::get_if<JPH::AxisConstraintPart>(&row.part)) {
            linear->WarmStart(*mBody1, *mBody2, row.normal, ratio);
        } else {
            std::get<JPH::AngleConstraintPart>(row.part).WarmStart(*mBody1, *mBody2, ratio);
        }
    }
}

bool SingleTrackJointConstraint::SolveVelocityConstraint(float) {
    bool changed = false;
    for (auto& row : rows_) {
        changed |= row.Solve(*mBody1, *mBody2, lever_a_, lever_b_);
    }
    return changed;
}

bool SingleTrackJointConstraint::SolvePositionConstraint(float, float baumgarte) {
    if (first_hard_ == rows_.size()) {
        return false;
    }
    const auto state = Measure(false);
    bool changed = false;
    for (size_t i = first_hard_; i < rows_.size(); ++i) {
        const unsigned slot = rows_[i].slot;
        const auto& coordinate = state.coordinates[slot];
        float value = coordinate.value;
        if (Periodic(definition_.kind, slot) && definition_.axes[slot].limit == Limit::kHard) {
            value = Unwrap(value, definition_.axes[slot]);
        }
        const auto [lo, hi] = Bounds(definition_, slot, a_scale_);
        const float error = value - std::clamp(value, lo, hi);
        if (error == 0) {
            continue;
        }
        const float length = coordinate.gradient.Length();
        if (length > 0) {
            JPH::AngleConstraintPart part;
            part.CalculateConstraintProperties(*mBody1, *mBody2, state.frame_a * (coordinate.gradient / length));
            changed |= part.SolvePositionConstraint(*mBody1, *mBody2, error / length, baumgarte);
        }
    }
    return changed;
}

void SingleTrackJointConstraint::SaveState(JPH::StateRecorder& stream) const {
    TwoBodyConstraint::SaveState(stream);
    for (const auto& row : rows_) {
        stream.Write(row.side);
        std::visit([&](const auto& p) { p.SaveState(stream); }, row.part);
    }
}

void SingleTrackJointConstraint::RestoreState(JPH::StateRecorder& stream) {
    TwoBodyConstraint::RestoreState(stream);
    for (auto& row : rows_) {
        stream.Read(row.side);
        std::visit([&](auto& p) { p.RestoreState(stream); }, row.part);
    }
}

}  // namespace ysm::physics
