project(ysm-codec CXX)

file(GLOB_RECURSE codec_code_files CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_library(ysm-codec STATIC ${codec_code_files})
unset(codec_code_files)
set_property(TARGET ysm-codec PROPERTY CXX_STANDARD 23)
target_include_directories(ysm-codec PUBLIC include PRIVATE src)

if (YSM_ENABLE_DEBUG_LOG)
    target_compile_definitions(ysm-codec PRIVATE
            YSM_ENABLE_DEBUG_LOG=1
            ABSL_MAX_VLOG_VERBOSITY=2)
else ()
    target_compile_definitions(ysm-codec PRIVATE
            YSM_ENABLE_DEBUG_LOG=0
            ABSL_MAX_VLOG_VERBOSITY=0)
endif ()

target_link_libraries(ysm-codec
        PUBLIC
        ysm-core
        optional_ref
        PRIVATE
        msft_proxy4
        minizip
        lzma
        zlib-ng-static
        libzstd_static
        yuv
        avif
        dav1d
        turbojpeg-static
        spng_static
        webp
        opus
        Ogg::ogg
        Vorbis::vorbis
        SpeexDSP::resampler
        blake3
        cryptopp)
