import os

from conan import ConanFile
from conan.tools.files import copy


class VorbisConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "vorbis"
    version = "1.3.7"
    license = "BSD-3-Clause"
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def package(self):
        super().package()
        copy(self, "COPYING", src=self._source_root(),
             dst=os.path.join(self.package_folder, "licenses"))
