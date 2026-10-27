project(ysm-lib-jni CXX)

file(GLOB_RECURSE legacyJniCodeFiles CONFIGURE_DEPENDS
        src/legacy/*.cpp src/legacy/*.cc)
add_library(ysm-legacy-jni OBJECT ${legacyJniCodeFiles})
set_property(TARGET ysm-legacy-jni PROPERTY CXX_STANDARD 23)
set_property(TARGET ysm-legacy-jni PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(ysm-legacy-jni PRIVATE src)
target_link_libraries(ysm-legacy-jni PRIVATE ysm-core ysm-java ysm-legacy)

file(GLOB_RECURSE libJniCodeFiles CONFIGURE_DEPENDS src/*.cpp src/*.cc)
list(FILTER libJniCodeFiles EXCLUDE REGEX "[/\\\\]legacy[/\\\\]")
if (YSM_MACOS)
    if (NOT CMAKE_OBJCXX_COMPILER)
        set(CMAKE_OBJCXX_COMPILER "${CMAKE_CXX_COMPILER}")
    endif ()
    if (NOT CMAKE_OBJCXX_COMPILER_TARGET)
        set(CMAKE_OBJCXX_COMPILER_TARGET "${CMAKE_CXX_COMPILER_TARGET}")
    endif ()
    enable_language(OBJCXX)
    list(APPEND libJniCodeFiles src/runtime/mac_user_directories.mm)
endif ()
add_library(ysm-lib-jni SHARED ${libJniCodeFiles}
        $<TARGET_OBJECTS:ysm-legacy-jni>)
if (YSM_MACOS)
    set(exportedSymbolsFile "${CMAKE_CURRENT_BINARY_DIR}/exported-symbols.txt")
endif ()
include(abi-version.cmake)
set_property(TARGET ysm-lib-jni PROPERTY CXX_STANDARD 23)
if (YSM_MACOS)
    set_target_properties(ysm-lib-jni PROPERTIES
            OBJCXX_STANDARD 23
            OBJCXX_VISIBILITY_PRESET hidden)
    find_library(ysmFoundationFramework Foundation REQUIRED)
    target_link_libraries(ysm-lib-jni PRIVATE "${ysmFoundationFramework}")
endif ()
set_target_properties(ysm-lib-jni PROPERTIES OUTPUT_NAME "ysm")
if (YSM_ENABLE_DEBUG_LOG)
    target_compile_definitions(ysm-legacy-jni PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
    target_compile_definitions(ysm-lib-jni PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
else ()
    target_compile_definitions(ysm-legacy-jni PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
    target_compile_definitions(ysm-lib-jni PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
endif ()

target_link_libraries(ysm-lib-jni PRIVATE
        ysm-core
        ysm-gfx
        ysm-codec
        ysm-java
        ysm-legacy
        yalantinglibs
        msft_proxy4
        pystring)
if (YSM_WINDOWS)
    target_link_options(ysm-lib-jni PRIVATE
            /MAP:${CMAKE_CURRENT_BINARY_DIR}/ysm.map)
elseif (YSM_LINUX OR YSM_ANDROID)
    target_link_options(ysm-lib-jni PRIVATE
            -Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/version-map.txt
            -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/ysm.map)
elseif (YSM_MACOS)
    target_link_options(ysm-lib-jni PRIVATE
            "-Wl,-exported_symbols_list,${exportedSymbolsFile}")
endif ()
