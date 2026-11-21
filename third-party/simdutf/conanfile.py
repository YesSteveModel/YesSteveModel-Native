import os

from conan import ConanFile
from conan.tools.files import copy


class SimdutfConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    name = "simdutf"
    version = "9.4.0"
    license = "Apache-2.0 OR MIT"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def package(self):
        super().package()
        copy(self, "LICENSE-*", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
