set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded$<$<CONFIG:Debug>:Debug>)

add_compile_definitions(
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    _AMD64_
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
    _CRT_SECURE_NO_WARNINGS
)

if (CMAKE_C_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_c_compile_options(/Zc:preprocessor)
endif ()

add_c_compile_options(
    /utf-8
    /permissive-
    /volatile:iso
)

if(NOT ysm_in_try_compile)
    add_c_compile_options(LLVM -march=x86-64)
endif()

add_cxx_compile_options(/EHsc)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    add_c_compile_options(
        /Gw
        LLVM
        -O3
        -fomit-frame-pointer
        -flto=thin
        -funique-source-file-names
        -fsplit-lto-unit
    )
    add_cxx_compile_options(
        /EHsc
        LLVM
        -fwhole-program-vtables
        -fforce-emit-vtables
    )
    add_custom_link_options(
        /OPT:REF
        /OPT:ICF
    )
endif()
