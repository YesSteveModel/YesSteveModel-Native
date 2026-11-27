add_library(ysm-physics STATIC
        src/runtime.cc
        src/physics_asset.cc
        src/physics_shape.cc
        src/physics_world.cc
        src/bone_sample.cc
        src/native_joint.cc
        src/single_track_joint_constraint.cc
        src/worker_scratch.cc)
set_target_properties(ysm-physics PROPERTIES
        CXX_STANDARD 23
        POSITION_INDEPENDENT_CODE ON)
target_include_directories(ysm-physics PUBLIC include)
target_link_libraries(ysm-physics
        PUBLIC ysm-core
        PRIVATE Jolt::Jolt ysm-model ysm-proto "$<COMPILE_ONLY:ysm-gfx>")
