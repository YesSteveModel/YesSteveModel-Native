project(ysm-gfx CXX)

file(GLOB_RECURSE gfx_code_files CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_library(ysm-gfx STATIC ${gfx_code_files})
unset(gfx_code_files)
set_property(TARGET ysm-gfx PROPERTY CXX_STANDARD 23)
target_include_directories(ysm-gfx PUBLIC include PRIVATE src)

add_library(ysm-gfx-internal INTERFACE)
target_include_directories(ysm-gfx-internal INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_link_libraries(ysm-gfx-internal INTERFACE ysm-gfx)

if (YSM_ENABLE_DEBUG_LOG)
    target_compile_definitions(ysm-gfx PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
else ()
    target_compile_definitions(ysm-gfx PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
endif ()

target_link_libraries(ysm-gfx
        PUBLIC
        ysm-core
        cglm
        PRIVATE
        ysm-proto
        yalantinglibs)
