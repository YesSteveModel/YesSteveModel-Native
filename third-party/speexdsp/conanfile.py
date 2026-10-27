import os

from conan import ConanFile
from conan.tools.files import copy


class SpeexDspConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "speexdsp"
    version = "1.2.1"
    license = "BSD-3-Clause"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def package(self):
        super().package()
        copy(self, "COPYING", src=self._source_root(),
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        super().package_info()
        self.cpp_info.defines = ["OUTSIDE_SPEEX", "RANDOM_PREFIX=ysm_speex"]
