add_library(ysm-model STATIC src/bone_hierarchy.cc)
set_target_properties(ysm-model PROPERTIES
        CXX_STANDARD 23
        POSITION_INDEPENDENT_CODE ON)
target_include_directories(ysm-model PUBLIC include)
target_link_libraries(ysm-model PUBLIC absl::statusor PRIVATE absl::flat_hash_map ysm-proto)
