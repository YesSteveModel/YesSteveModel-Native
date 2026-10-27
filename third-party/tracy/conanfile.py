from conan import ConanFile


class TracyConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "tracy"
    version = "0.14.1"
    license = "BSD-3-Clause"
    description = "A real time, nanosecond resolution, remote telemetry profiler"
    homepage = "https://github.com/wolfpld/tracy"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def requirements(self):
        self.requires(
            "mimalloc/3.6.0@ysm/stable",
            transitive_headers=True,
            transitive_libs=True,
        )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def package_info(self):
        super().package_info()
        if self._target_os == "windows":
            self.cpp_info.system_libs = ["ws2_32", "dbghelp", "secur32", "advapi32"]
        elif self._target_os == "linux":
            self.cpp_info.system_libs = ["pthread", "dl"]
        elif self._target_os == "android":
            self.cpp_info.system_libs = ["dl"]
