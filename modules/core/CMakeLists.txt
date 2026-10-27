project (ysm-core CXX)

FILE(GLOB_RECURSE coreCodeFiles CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_library(ysm-core STATIC ${coreCodeFiles})
unset(coreCodeFiles)
set_property(TARGET ysm-core PROPERTY CXX_STANDARD 23)
target_include_directories(ysm-core PUBLIC src)

# debug logging
target_compile_definitions(ysm-core PUBLIC $<$<CONFIG:Debug>:YSM_DEBUG=1>)
if (YSM_ENABLE_DEBUG_LOG)
    target_compile_definitions(ysm-core PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
else ()
    target_compile_definitions(ysm-core PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
endif ()

# common deps
target_link_libraries(ysm-core PUBLIC
        mimalloc-static

        optional_ref
        magic_enum

        cpu_features

        absl::inlined_vector
        absl::flat_hash_map
        absl::status
        absl::statusor
        absl::log_globals
        absl::log_initialize
        absl::log
        absl::time
)
target_link_libraries(ysm-core PRIVATE absl::function_ref)

# tracy
if (YSM_ENABLE_TRACY)
    target_link_libraries(ysm-core PUBLIC Tracy::TracyClient)
    target_compile_definitions(ysm-core PUBLIC
            YSM_ENABLE_TRACY=1
            TRACY_ENABLE
            TRACY_ON_DEMAND
            TRACY_NO_CALLSTACK
            TRACY_NO_CODE_TRANSFER
            TRACY_NO_CONTEXT_SWITCH
            TRACY_NO_SAMPLING
            TRACY_NO_VSYNC_CAPTURE
            TRACY_NO_FRAME_IMAGE
            TRACY_NO_SYSTEM_TRACING
            TRACY_NO_CRASH_HANDLER)
endif ()

# system
if (YSM_LINUX OR YSM_MACOS)
    find_package(Iconv REQUIRED)
    target_link_libraries(ysm-core PRIVATE Iconv::Iconv)
elseif (YSM_WINDOWS)
    target_link_libraries(ysm-core PRIVATE synchronization) # TODO: 适配 win7 后要处理
endif ()
