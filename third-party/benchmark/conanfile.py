from conan import ConanFile


class GoogleBenchmarkConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "benchmark"
    version = "1.10.0"
    license = "Apache-2.0"
    description = "A microbenchmark support library"
    homepage = "https://github.com/google/benchmark"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def package_info(self):
        super().package_info()

        benchmark = self.cpp_info.components["benchmark"]
        benchmark.defines = ["BENCHMARK_STATIC_DEFINE"]
        if self._target_os == "windows":
            benchmark.system_libs = ["shlwapi"]
        elif self._target_os == "linux":
            benchmark.system_libs = ["pthread", "rt"]
        elif self._target_os == "freebsd":
            benchmark.system_libs = ["pthread"]
        elif self._target_os == "sunos":
            benchmark.system_libs = ["pthread", "rt", "kstat"]

        self.cpp_info.components["benchmark_main"].requires = ["benchmark"]
