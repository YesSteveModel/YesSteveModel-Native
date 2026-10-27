from conan import ConanFile


class GoogleTestConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "googletest"
    version = "1.18.0"
    license = "BSD-3-Clause"
    description = "GoogleTest testing and mocking framework"
    homepage = "https://github.com/google/googletest"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def requirements(self):
        self.requires(
            "abseil/20260817.0@ysm/stable",
            transitive_headers=True,
            transitive_libs=True,
        )
        self.requires(
            "re2/2025-11-05@ysm/stable",
            transitive_headers=True,
            transitive_libs=True,
        )

    def package_info(self):
        super().package_info()
        gtest = self.cpp_info.components["gtest"]
        gtest.defines = ["GTEST_HAS_ABSL=1"]
        gtest.requires = [
            "abseil::absl_failure_signal_handler",
            "abseil::absl_stacktrace",
            "abseil::absl_symbolize",
            "abseil::absl_flags_parse",
            "abseil::absl_flags_reflection",
            "abseil::absl_flags_usage",
            "abseil::absl_strings",
            "re2::re2",
        ]
        self.cpp_info.components["gtest_main"].requires = ["gtest"]
