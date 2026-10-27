from conan import ConanFile


class YuvConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "yuv"
    version = "0.0.1974"
    license = ""
    package_type = "static-library"

    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}
