project(ysm-test CXX)

file(GLOB_RECURSE test_code_files CONFIGURE_DEPENDS src/*.cpp src/*.cc)
add_executable(ysm-test ${test_code_files})
unset(test_code_files)
set_property(TARGET ysm-test PROPERTY CXX_STANDARD 23)
target_include_directories(ysm-test PRIVATE
        ../legacy/src
        ../lib-jni/src)
target_compile_definitions(ysm-test PRIVATE
        YSM_ENABLE_DEBUG_LOG=1
        ABSL_MAX_VLOG_VERBOSITY=2
)
target_link_libraries(ysm-test PRIVATE
        ysm-proto
        ysm-core
        ysm-gfx-internal
        ysm-codec
        ysm-java
        ysm-legacy
        ysm-legacy-jni
        yalantinglibs
        GTest::gtest
        opus
        cryptopp)

include(GoogleTest)
if(CMAKE_CROSSCOMPILING)
    gtest_add_tests(TARGET ysm-test)
else()
    gtest_discover_tests(ysm-test DISCOVERY_MODE PRE_TEST)
endif()
