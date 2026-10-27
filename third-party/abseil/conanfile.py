import json
import os
import re

from conan import ConanFile
from conan.tools.apple import is_apple_os
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain
from conan.tools.files import copy, load, rmdir, save


class AbseilConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "abseil"
    version = "20260817.0"
    license = "Apache-2.0"
    description = "Abseil Common Libraries (C++) from Google"
    homepage = "https://github.com/abseil/abseil-cpp"
    package_type = "static-library"
    short_paths = True
    extension_properties = {"compatibility_cppstd": False}

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def validate(self):
        check_min_cppstd(self, 17)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        tc = CMakeToolchain(self)
        tc.variables["YSM_REQUIRED_PACKAGES"] = ";".join(
            deps.get_cmake_package_name(dependency)
            for dependency in self.dependencies.direct_host.values()
        )
        tc.variables["YSM_GLOBAL_LINK_TARGETS"] = ";".join(
            self._build_data.get("global_link_targets", [])
        )
        for key, value in self._build_data.get("options", {}).items():
            tc.cache_variables[key] = value
        if self.settings.os == "Windows" and self.settings.compiler in ["msvc", "clang"]:
            runtime = str(self.settings.get_safe("compiler.runtime", ""))
            if runtime:
                tc.cache_variables["ABSL_MSVC_STATIC_RUNTIME"] = runtime == "static"
        tc.generate()

    def package(self):
        copy(self, "LICENSE", src=self._source_root(), dst=os.path.join(self.package_folder, "licenses"))

        cmake = CMake(self)
        cmake.install()

        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))

        cmake_folder = os.path.join(self.package_folder, "lib", "cmake")
        absl_targets_file = os.path.join(cmake_folder, "absl", "abslTargets.cmake")
        components = self._load_components_from_cmake_target_file(absl_targets_file)
        rmdir(self, cmake_folder)

        save(self, self._components_helper_filepath, json.dumps(components, indent=4))

    def _load_components_from_cmake_target_file(self, absl_target_file_path):
        components = {}
        content = load(self, absl_target_file_path).replace("\r\n", "\n")
        cmake_functions = re.findall(
            r"(?P<func>add_library|set_target_properties)[\n|\s]*\([\n|\s]*(?P<args>[^)]*)\)",
            content,
        )
        for function_name, function_args in cmake_functions:
            function_args = re.split(r"[\s|\n]+", function_args, maxsplit=2)
            cmake_target_name = function_args[0]
            cmake_target = cmake_target_name.replace("absl::", "")
            component_name = "absl_" + cmake_target

            component = components.setdefault(component_name, {"cmake_target": cmake_target})

            if function_name == "add_library":
                imported_target_type = function_args[1]
                if imported_target_type in ["STATIC", "SHARED"]:
                    component["libs"] = [component_name] if cmake_target != "abseil_dll" else ["abseil_dll"]
            elif function_name == "set_target_properties":
                target_properties = re.findall(
                    r"(?P<property>INTERFACE_COMPILE_DEFINITIONS|INTERFACE_INCLUDE_DIRECTORIES|INTERFACE_LINK_LIBRARIES)[\n|\s]+(?P<values>.+)",
                    function_args[2],
                )
                for property_type, values in target_properties:
                    values_list = values.replace('"', "").split(";")
                    if property_type == "INTERFACE_LINK_LIBRARIES":
                        self._read_component_link_libraries(component, values_list)
                    elif property_type == "INTERFACE_COMPILE_DEFINITIONS":
                        for definition in values_list:
                            if definition == r"\$<\$<PLATFORM_ID:AIX>:_LINUX_SOURCE_COMPAT>":
                                if self.settings.os == "AIX":
                                    component.setdefault("defines", []).append("_LINUX_SOURCE_COMPAT")
                            else:
                                component.setdefault("defines", []).append(definition)

        return components

    def _read_component_link_libraries(self, component, values):
        for dependency in values:
            if dependency.startswith("absl::"):
                component.setdefault("requires", []).append(dependency.replace("absl::", "absl_"))
            elif dependency == "mimalloc-static":
                component.setdefault("requires", []).append("mimalloc::mimalloc")
            elif self.settings.os in ["Linux", "FreeBSD"]:
                if dependency == "Threads::Threads":
                    component.setdefault("system_libs", []).append("pthread")
                elif "-lm" in dependency:
                    component.setdefault("system_libs", []).append("m")
                elif "-lrt" in dependency:
                    component.setdefault("system_libs", []).append("rt")
            elif self.settings.os == "Windows":
                for system_lib in ["bcrypt", "advapi32", "dbghelp"]:
                    if system_lib in dependency:
                        component.setdefault("system_libs", []).append(system_lib)
            elif is_apple_os(self):
                if "CoreFoundation" in dependency:
                    component.setdefault("frameworks", []).append("CoreFoundation")

    @property
    def _components_helper_filepath(self):
        return os.path.join(self.package_folder, "lib", "components.json")

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "absl")

        abseil_components = json.loads(load(self, self._components_helper_filepath))
        for component_name, values in abseil_components.items():
            component = self.cpp_info.components[component_name]
            component.set_property("cmake_target_name", f"absl::{values['cmake_target']}")
            component.set_property("pkg_config_name", component_name)
            component.libs = values.get("libs", [])
            component.defines = values.get("defines", [])
            component.system_libs = values.get("system_libs", [])
            component.frameworks = values.get("frameworks", [])
            component.requires = values.get("requires", [])
