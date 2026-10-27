from conan import ConanFile


class ProxyConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    name = "proxy"
    version = "4.1.0"
    license = "MIT"
    package_type = "header-library"
