#pragma once

#include "physics_world_internal.h"

namespace ysm::physics {
struct PhysicsWorldTestAccess {
    static auto& Get(PhysicsWorld& world) { return *world.impl_; }
};

namespace test {
struct DebugSnapshot {
    explicit DebugSnapshot(const PhysicsWorld& world) {
        const auto layout = world.DebugSnapshotLayout();
        floats.resize(layout.float_count);
        ints.resize(layout.int_count);
        flags = world.ReadDebugSnapshot({floats, ints});
    }

    std::vector<float> floats;
    std::vector<int32_t> ints;
    int32_t flags;
};

inline schema::PhysicsNode Ball(std::string name, schema::PhysicsNodeKind kind = schema::PhysicsNodeKind::kDynamic,
                                float radius = 8, std::vector<float> center = {0, 0, 0}) {
    schema::PhysicsNode n{.id = name, .bone = name, .kind = kind};
    if (kind != schema::PhysicsNodeKind::kAnchor) {
        n.shapes.push_back({schema::SphereShape{std::move(center), radius}});
    }
    if (kind == schema::PhysicsNodeKind::kDynamic) {
        n.mass = 1;
        n.angular_damping = n.linear_damping = 0;
    }
    return n;
}

inline schema::PhysicsConstraint Link(schema::PhysicsConstraintKind kind, std::string a, std::string b) {
    schema::ConstraintFrame basis{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    return {.id = a + "-" + b, .node_a = std::move(a), .node_b = std::move(b),
            .kind = kind, .frame_a = basis, .frame_b = basis};
}
}  // namespace test
}  // namespace ysm::physics
