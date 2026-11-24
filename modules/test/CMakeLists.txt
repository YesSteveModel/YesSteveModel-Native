project(ysm-test CXX)

file(GLOB_RECURSE test_code_files CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_executable(ysm-test ${test_code_files})
unset(test_code_files)
set_property(TARGET ysm-test PROPERTY CXX_STANDARD 23)
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # This translation unit exceeds the section limit of standard COFF objects.
    set_source_files_properties(src/legacy_historical_decoder_test.cc PROPERTIES COMPILE_OPTIONS "/bigobj")
endif ()
target_include_directories(ysm-test PRIVATE
        ../legacy/src
        ../lib-jni/src)
target_compile_definitions(ysm-test PRIVATE
        YSM_ENABLE_DEBUG_LOG=1
        ABSL_MAX_VLOG_VERBOSITY=2
        YSM_TEST_STORAGE_DEBUG_LOG=$<BOOL:${YSM_ENABLE_DEBUG_LOG}>
)
target_link_libraries(ysm-test PRIVATE
        ysm-proto
        ysm-core
        ysm-gfx-internal
        ysm-codec
        ysm-java
        ysm-storage
        SQLiteCpp::SQLite3
        ysm-legacy
        ysm-legacy-jni
        yalantinglibs
        GTest::gtest
        opus
        cryptopp)

include(GoogleTest)
set(fixture_dir_arg "--fixture-dir=${CMAKE_CURRENT_SOURCE_DIR}/data")
if(CMAKE_CROSSCOMPILING)
    gtest_add_tests(TARGET ysm-test EXTRA_ARGS "${fixture_dir_arg}")
else()
    gtest_discover_tests(ysm-test DISCOVERY_MODE PRE_TEST
                         EXTRA_ARGS "${fixture_dir_arg}")
endif()
