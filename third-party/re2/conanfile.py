from conan import ConanFile


class Re2Conan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "re2"
    version = "2025-11-05"
    license = "BSD-3-Clause"
    description = "RE2 regular expression library"
    homepage = "https://github.com/google/re2"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def requirements(self):
        self.requires(
            "abseil/20260817.0@ysm/stable",
            transitive_headers=True,
            transitive_libs=True,
        )

    def package_info(self):
        super().package_info()
        re2 = self.cpp_info.components["re2"]
        re2.requires = [
            "abseil::absl_absl_check",
            "abseil::absl_absl_log",
            "abseil::absl_base",
            "abseil::absl_core_headers",
            "abseil::absl_fixed_array",
            "abseil::absl_flags",
            "abseil::absl_flat_hash_map",
            "abseil::absl_flat_hash_set",
            "abseil::absl_hash",
            "abseil::absl_inlined_vector",
            "abseil::absl_optional",
            "abseil::absl_span",
            "abseil::absl_str_format",
            "abseil::absl_strings",
            "abseil::absl_synchronization",
        ]
        if self.settings.os in ["Linux", "FreeBSD"]:
            re2.system_libs = ["pthread"]
