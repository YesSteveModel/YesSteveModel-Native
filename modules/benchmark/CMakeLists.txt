project(ysm-benchmark CXX)

file(GLOB_RECURSE benchmark_code_files CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_executable(ysm-benchmark ${benchmark_code_files})
unset(benchmark_code_files)
set_property(TARGET ysm-benchmark PROPERTY CXX_STANDARD 23)
target_link_libraries(ysm-benchmark PRIVATE
        ysm-proto
        ysm-core
        ysm-gfx-internal
        benchmark::benchmark)

if (YSM_X64)
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(src/cglm_avx.cc PROPERTIES
                COMPILE_OPTIONS "/clang:-mavx2;/clang:-mfma")
    else ()
        set_source_files_properties(src/cglm_avx.cc PROPERTIES
                COMPILE_OPTIONS "-mavx2;-mfma")
    endif ()
endif ()

# Keep benchmark-only Java classes out of the production JNI registry.
add_library(ysm-jni-benchmark SHARED jni/entry.cc)
set_property(TARGET ysm-jni-benchmark PROPERTY CXX_STANDARD 23)
target_link_libraries(ysm-jni-benchmark PRIVATE ysm-java mimalloc-static)
if (YSM_ENABLE_DEBUG_LOG)
    target_compile_definitions(ysm-jni-benchmark PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
else ()
    target_compile_definitions(ysm-jni-benchmark PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
endif ()
