project(ysm-storage CXX)

file(GLOB_RECURSE storage_code_files CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_library(ysm-storage STATIC ${storage_code_files})
set_property(TARGET ysm-storage PROPERTY CXX_STANDARD 23)
target_include_directories(ysm-storage PUBLIC include)
target_link_libraries(ysm-storage PUBLIC ysm-core PRIVATE SQLiteCpp::SQLiteCpp mimalloc-static)

if (YSM_ENABLE_DEBUG_LOG)
    target_compile_definitions(ysm-storage PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
else ()
    target_compile_definitions(ysm-storage PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
endif ()
