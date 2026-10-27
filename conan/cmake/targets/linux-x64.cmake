if(NOT ysm_in_try_compile)
    add_c_compile_options(-march=x86-64)
endif()

add_compile_definitions(
    _GNU_SOURCE=1
    _FILE_OFFSET_BITS=64
    _TIME_BITS=64
)

add_c_compile_options(-pthread)
add_custom_link_options(
    -static-libstdc++
    -static-libgcc
    -ldl
    -pthread
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
        -Wl,--gc-sections,--no-undefined,-z,relro
        LLVM
        -Wl,--icf=all,--lto-whole-program-visibility
    )
endif()
