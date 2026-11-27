#include "physics_asset_internal.h"

#include <cmath>
#include <limits>
#include <numbers>

#include <absl/strings/str_cat.h>
#include <Jolt/Math/DVec3.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

namespace ysm::physics {
namespace {
using Matrix = std::array<std::array<double, 3>, 3>;

struct Primitive {
    JPH::RefConst<JPH::Shape> shape;
    JPH::DVec3 center = JPH::DVec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();
    double volume = 0;
    JPH::DVec3 inertia_per_kg = JPH::DVec3::sZero();
};

JPH::DVec3 Meters(const std::vector<float>& pixels) {
    return JPH::DVec3(pixels[0], pixels[1], pixels[2]) * kPixelToMeter;
}

Primitive MakeSphere(JPH::DVec3Arg center, double radius) {
    if (!(static_cast<float>(radius) > 0)) {
        return {};
    }
    return {.shape = new JPH::SphereShape(static_cast<float>(radius)),
            .center = center,
            .volume = (4.0 / 3.0) * std::numbers::pi * radius * radius* radius,
            .inertia_per_kg = JPH::DVec3::sReplicate(0.4 * radius * radius)};
}

Primitive MakePrimitive(const schema::SphereShape& sphere) {
    return MakeSphere(Meters(sphere.center), static_cast<double>(sphere.radius) * kPixelToMeter);
}

Primitive MakePrimitive(const schema::BoxShape& box) {
    constexpr float kConvexRadius = 0.002f;
    const auto size = Meters(box.size);
    const auto half_extent = static_cast<JPH::Vec3>(size * 0.5);
    if (!(half_extent.ReduceMin() > 0)) {
        return {};
    }
    const auto [x, y, z] = std::array{size.GetX(), size.GetY(), size.GetZ()};
    const auto& q = box.rotation;
    return {.shape = new JPH::BoxShape(half_extent, kConvexRadius),
            .center = Meters(box.center),
            .rotation = JPH::Quat(q[0], q[1], q[2], q[3]).Normalized(),
            .volume = x * y * z,
            .inertia_per_kg = JPH::DVec3(y * y + z * z, x * x + z * z, x * x + y * y) / 12.0};
}

Primitive MakePrimitive(const schema::CapsuleShape& capsule) {
    const double radius = static_cast<double>(capsule.radius) * kPixelToMeter;
    const auto a = Meters(capsule.point_a);
    const auto axis = Meters(capsule.point_b) - a;
    const auto center = a + axis * 0.5;
    const double length = axis.Length();
    const auto half_height = static_cast<float>(length * 0.5);
    if (half_height <= 0) {
        return MakeSphere(center, radius);
    }
    if (!(static_cast<float>(radius) > 0) || !std::isfinite(half_height)) {
        return {};
    }
    const auto y = static_cast<JPH::Vec3>(axis / length);
    const auto x = y.GetNormalizedPerpendicular();
    const JPH::Mat44 axes(JPH::Vec4(x, 0), JPH::Vec4(y, 0), JPH::Vec4(x.Cross(y), 0), JPH::Vec4(0, 0, 0, 1));
    const double r2 = radius * radius;
    const double cylinder = std::numbers::pi * r2 * length;
    const double caps = (4.0 / 3.0) * std::numbers::pi * r2 * radius;
    const double volume = cylinder + caps;
    const double axial = r2 * (0.5 * cylinder + 0.4 * caps);
    // Each cap's centroid lies 3r/8 beyond its cylinder end.
    const double radial = cylinder * (0.25 * r2 + length * length / 12.0) +
                          caps * (0.4 * r2 + 0.25 * length * length + 0.375 * length * radius);
    return {.shape = new JPH::CapsuleShape(half_height, static_cast<float>(radius)),
            .center = center,
            .rotation = axes.GetQuaternion().Normalized(),
            .volume = volume,
            .inertia_per_kg = JPH::DVec3(radial, axial, radial) / volume};
}

void AccumulateInertia(Matrix& tensor, const Primitive& part, JPH::DVec3Arg center) {
    const auto axis = JPH::DVec3(part.rotation.GetXYZ());
    const double w = part.rotation.GetW();
    const double twice_inv_norm = 2.0 / (axis.LengthSq() + w * w);
    const std::array local{JPH::DVec3::sAxisX(), JPH::DVec3::sAxisY(), JPH::DVec3::sAxisZ()};
    std::array<JPH::DVec3, 3> rotated;
    for (int i = 0; i < 3; ++i) {
        rotated[i] = local[i] + twice_inv_norm * axis.Cross(axis.Cross(local[i]) + w * local[i]);
    }
    const auto d = part.center - center;
    const auto squared = d * d;
    const JPH::DVec3 transverse(squared.GetY() + squared.GetZ(), squared.GetZ() + squared.GetX(),
                                squared.GetX() + squared.GetY());
    for (int i = 0; i < 3; ++i) {
        for (int j = i; j < 3; ++j) {
            double entry = i == j ? transverse[i] : -d[i] * d[j];
            for (int k = 0; k < 3; ++k) {
                entry += part.inertia_per_kg[k] * rotated[k][i] * rotated[k][j];
            }
            tensor[i][j] += part.volume * entry;
            tensor[j][i] = tensor[i][j];
        }
    }
}

// Cyclic Jacobi rotations preserve a right-handed principal frame even for repeated moments.
JPH::Quat Diagonalize(Matrix& tensor) {
    Matrix axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (int sweep = 0; sweep < 16; ++sweep) {
        bool rotated = false;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const double off = tensor[p][q];
                const double tolerance =
                    std::numeric_limits<double>::epsilon() * std::sqrt(std::abs(tensor[p][p] * tensor[q][q]));
                if (std::abs(off) <= tolerance) {
                    continue;
                }
                const double gap = 0.5 * (tensor[q][q] - tensor[p][p]);
                const double t = off / (gap + std::copysign(std::hypot(gap, off), gap));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;
                tensor[p][p] -= t * off;
                tensor[q][q] += t * off;
                tensor[p][q] = tensor[q][p] = 0;
                const int r = 3 - p - q;
                const double rp = tensor[r][p], rq = tensor[r][q];
                tensor[r][p] = tensor[p][r] = c * rp - s * rq;
                tensor[r][q] = tensor[q][r] = s * rp + c * rq;
                for (auto& row : axes) {
                    const double u = row[p], v = row[q];
                    row[p] = c * u - s * v;
                    row[q] = s * u + c * v;
                }
                rotated = true;
            }
        }
        if (!rotated) {
            break;
        }
    }
    const auto column = [&](int i) {
        return JPH::Vec4(static_cast<float>(axes[0][i]), static_cast<float>(axes[1][i]),
                         static_cast<float>(axes[2][i]), 0);
    };
    return JPH::Mat44(column(0), column(1), column(2), JPH::Vec4(0, 0, 0, 1)).GetQuaternion().Normalized();
}
}  // namespace

absl::Status CompileNodeShape(const schema::PhysicsNode& source, Node& node) {
    std::vector<Primitive> parts;
    parts.reserve(source.shapes.size());
    double volume = 0;
    auto center = JPH::DVec3::sZero();
    for (const auto& shape : source.shapes) {
        const auto& part = parts.emplace_back(
            std::visit([](const auto& entry) { return MakePrimitive(entry); }, shape.shape));
        if (part.shape == nullptr) {
            return absl::InvalidArgumentError(absl::StrCat("Invalid shape for ", source.id));
        }
        volume += part.volume;
        center += part.volume * part.center;
    }
    if (!(volume > 0) || !std::isfinite(volume)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid shape volume for ", source.id));
    }

    const bool single = parts.size() == 1;
    center = single ? parts[0].center : center / volume;
    node.com_in_bone = static_cast<JPH::Vec3>(center);
    if (single) {
        node.shape = parts[0].shape;
        node.shape_from_bone = {node.com_in_bone, parts[0].rotation};
    } else {
        JPH::StaticCompoundShapeSettings compound;
        for (const auto& part : parts) {
            compound.AddShape(static_cast<JPH::Vec3>(part.center), part.rotation, part.shape);
        }
        const auto shape = compound.Create();
        if (!shape.IsValid()) {
            return absl::InvalidArgumentError(absl::StrCat("Invalid compound for ", source.id));
        }
        node.shape = shape.Get();
    }
    if (node.kind != schema::PhysicsNodeKind::kDynamic) {
        return absl::OkStatus();
    }

    auto diagonal = parts[0].inertia_per_kg * node.mass;
    if (!single) {
        Matrix tensor{};
        for (const auto& part : parts) {
            AccumulateInertia(tensor, part, center);
        }
        node.base_inertia_rotation = Diagonalize(tensor);
        diagonal = JPH::DVec3(tensor[0][0], tensor[1][1], tensor[2][2]) * (node.mass / volume);
    }
    const auto positive_finite = [](float n) { return n > 0 && std::isfinite(n); };
    if (!positive_finite(node.mass) || !positive_finite(1.0f / node.mass)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid mass for ", source.id));
    }
    for (int i = 0; i < 3; ++i) {
        if (!positive_finite(static_cast<float>(diagonal[i])) ||
            !positive_finite(static_cast<float>(1.0 / diagonal[i]))) {
            return absl::InvalidArgumentError(absl::StrCat("Invalid inertia for ", source.id));
        }
    }
    node.base_inv_inertia_diagonal = static_cast<JPH::Vec3>(JPH::DVec3::sOne() / diagonal);
    return absl::OkStatus();
}
}  // namespace ysm::physics
