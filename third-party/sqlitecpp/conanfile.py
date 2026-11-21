from conan import ConanFile
from conan.tools.files import copy
import os


class SQLiteCppConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"
    name = "sqlitecpp"
    version = "3.4.0"
    license = "MIT", "LicenseRef-SQLite-Public-Domain"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "licenses/*"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def package_info(self):
        super().package_info()
        self.cpp_info.components["wrapper"].requires = ["sqlite3"]
        self.cpp_info.components["wrapper"].defines = ["SQLITE_OMIT_LOAD_EXTENSION"]
        self.cpp_info.components["sqlite3"].defines = ["SQLITE_OMIT_LOAD_EXTENSION"]
        if self._target_os in ("linux", "android"):
            self.cpp_info.components["sqlite3"].system_libs = ["pthread", "m"]

    def package(self):
        super().package()
        copy(self, "LICENSE.txt", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "SQLite-Public-Domain.txt", src=os.path.join(self.export_sources_folder, "licenses"),
             dst=os.path.join(self.package_folder, "licenses"))
