import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout

required_conan_version = ">=2.28"

class YsmConan(ConanFile):
    name = "ysm"
    version = "3.0"
    license = "Apache-2.0"
    package_type = "application"
    settings = "os", "arch", "compiler", "build_type"

    options = {"fPIC": [True, False]}
    default_options = {
        "fPIC": True
    }

    def requirements(self):
        self.requires("sqlitecpp/3.4.0@ysm/stable")
        self.requires("mimalloc/3.6.0@ysm/stable")
        self.requires("cpu_features/0.11.0@ysm/stable")
        self.requires("abseil/20260817.0@ysm/stable")
        self.requires("zlib-ng/2.3.3@ysm/stable")
        self.requires("minizip-ng/4.2.2@ysm/stable")
        self.requires("zstd/1.5.7@ysm/stable")
        self.requires("cglm/0.9.7@ysm/stable")
        self.requires("lzma/26.02@ysm/stable")
        self.requires("opus/1.6.1@ysm/stable")
        self.requires("ogg/1.3.6@ysm/stable")
        self.requires("vorbis/1.3.7@ysm/stable")
        self.requires("speexdsp/1.2.1@ysm/stable")
        self.requires("spng/0.7.4@ysm/stable")
        self.requires("jpeg-turbo/3.2.0@ysm/stable")
        self.requires("webp/1.6.0@ysm/stable")
        self.requires("yuv/0.0.1974@ysm/stable")
        self.requires("avif/1.4.2@ysm/stable")
        self.requires("blake3/1.8.7@ysm/stable")
        self.requires("cryptopp/8.9.0@ysm/stable")
        self.requires("proxy/4.1.0@ysm/stable")
        self.requires("pystring/1.2.0@ysm/stable")
        self.requires("simdutf/9.4.0@ysm/stable")
        self.requires("optional_ref/0.3.1@ysm/stable")
        self.requires("magic_enum/0.9.8@ysm/stable")
        self.requires("jni/8.0.0@ysm/stable")
        self.requires("yalantinglibs/0.6.1@ysm/stable")
        self.requires("tracy/0.14.1@ysm/stable")

    def build_requirements(self):
        self.test_requires("benchmark/1.10.0@ysm/stable")
        self.test_requires("googletest/1.18.0@ysm/stable")

    @property
    def _build_mode(self):
        return self.conf.get("user.ysm:build_mode", default="build", choices=("build", "test", "benchmark"))

    @property
    def _build_only(self):
        return self.conf.get("user.ysm:build_only", default=False, check_type=bool)

    def validate(self):
        mode = self._build_mode
        if mode == "build" or self._build_only:
            return
        if not can_run(self):
            raise ConanInvalidConfiguration(f"bootstrap {mode} cannot run this target on the build host. Use --build-only.")
        if mode == "test" and self.conf.get("tools.build:skip_test", check_type=bool):
            raise ConanInvalidConfiguration("bootstrap test requires tools.build:skip_test to be disabled")

    def layout(self):
        target_profile = self.conf.get("user.target:profile", default=None)
        if target_profile is None:
            raise ConanInvalidConfiguration("Config 'user.target:profile' not defined")
        target_profile = str(target_profile).lower()
        build_type = str(self.settings.build_type).lower()
        self._layout_profile = target_profile

        cmake_layout(self)
        self.folders.build_folder_vars = [
            "self._layout_profile",
            "settings.build_type",
        ]
        self.folders.build = os.path.join("build", f"{target_profile}-{build_type}")
        self.folders.generators = os.path.join(self.folders.build, "generators")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.presets_prefix = ""
        tc.cache_variables["YSM_DEPENDENCY_PROVIDER"] = "conan"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        mode = self._build_mode
        if mode == "test":
            cmake.configure(variables={
                "BUILD_TESTING": "ON",
                "CMAKE_CTEST_ARGUMENTS": "--output-on-failure;--no-tests=error",
            })
            cmake.build(target="ysm-test")
            if not self._build_only:
                cmake.test()
        elif mode == "benchmark":
            cmake.configure(variables={"YSM_BUILD_BENCHMARKS": "ON"})
            cmake.build(target="ysm-benchmark" if self._build_only else "run-ysm-benchmark")
        else:
            cmake.configure()
            cmake.build(target="ysm-lib-jni")
