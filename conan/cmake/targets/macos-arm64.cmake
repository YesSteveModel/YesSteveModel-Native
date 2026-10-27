if(NOT ysm_in_try_compile)
    add_c_compile_options(-mcpu=apple-m1)
endif()

add_c_compile_options(-pthread)
if (HAS_MACOS_STATIC_LIBCXX)
    # 依赖于自构建的 libc++_static.a ，官方 SDK 没给
    add_custom_link_options(
        -nostdlib++
        -lc++_static
    )
endif ()

if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    add_c_compile_options(
        -fdata-sections
        -ffunction-sections
        -fomit-frame-pointer
        LLVM
        -flto=thin
        -funique-source-file-names
    )
    add_cxx_compile_options(
        LLVM
        -fwhole-program-vtables
        -fforce-emit-vtables
    )
    add_custom_link_options(
        -Wl,-S,-x,-undefined,error,-dead_strip
        LLVM
        -fwhole-program-vtables
    )
    if (MACOS_SPLIT_LTO_FIXED)
        add_c_compile_options(LLVM -fsplit-lto-unit)
    else ()
        add_c_compile_options(LLVM -fno-split-lto-unit)
    endif ()
endif()
