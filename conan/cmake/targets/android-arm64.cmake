if(NOT ysm_in_try_compile)  # 太坑了
    add_c_compile_options(-march=armv8-a)
endif()

add_c_compile_options(-pthread)
add_custom_link_options(
    -static-libstdc++
    -ldl
    -llog
)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    add_c_compile_options(
        -fdata-sections
        -ffunction-sections
        -fomit-frame-pointer
        LLVM
        -flto=thin
        -funique-source-file-names
        -fsplit-lto-unit
    )
    add_cxx_compile_options(
        LLVM
        -fwhole-program-vtables
        -fforce-emit-vtables
    )
    add_custom_link_options(
        -s
        -Wl,--gc-sections,--no-undefined,-z,relro,-z,common-page-size=65536,-z,max-page-size=65536
        LLVM
        -Wl,--icf=all
    )

    if (HAS_ANDROID_LIBCXX_LTO)
        # 依赖于启用了 LTO 的 libc++，NDK 内的没有。
        add_custom_link_options(-Wl,--lto-whole-program-visibility)
    endif ()
endif()
