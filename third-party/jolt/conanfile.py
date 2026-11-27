import os

from conan import ConanFile
from conan.tools.files import apply_conandata_patches, copy, get


class JoltConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*"
    name = "jolt"
    version = "5.7.0"
    license = "MIT"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def source(self):
        source = self._source_data
        get(self, url=source["archive_url"], sha256=source["sha256"], strip_root=True)
        apply_conandata_patches(self)

    def package(self):
        super().package()
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        super().package_info()
        if self.settings.build_type == "Debug":
            self.cpp_info.defines.append("JPH_DEBUG")
        if self._target_os == "android":
            self.cpp_info.defines.append("JPH_TARGET_ANDROID")
        if self._target_os == "linux":
            self.cpp_info.system_libs.append("pthread")
