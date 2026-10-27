# 添加依赖与接入 mimalloc

本页给出复用公共配方添加依赖的完整输入、操作与限制，无需先阅读已有 recipe、CMake 实现或构建系统文档。上游的源码版本、构建选项、库名和许可证仍须依据上游发布内容确定。[Bootstrap、Conan 与 CMake 构建系统](build-system_cn.md)是可选的原理参考。

引入前评估许可证、体积、受支持平台、线程与初始化模型、异常/RTTI/ABI 假设和维护成本。需要编译的生产依赖必须从源码构建并静态链接，纯头文件依赖按下文打包，最终只交付一个动态库；公开接口的依赖传播规则见下文。

## 检查上游更新

在项目根目录运行 `python third-party/check_updates.py`。脚本需要 Python 3.10+、Git、PyYAML 和 packaging。缺少 Python 包时，运行 `python -m pip install PyYAML packaging`。

脚本检查各配方 `conandata.yml` 的全部 `sources` 版本及 `extra_git`。可在命令后追加依赖目录名，限制检查范围，例如 `python third-party/check_updates.py optional_ref yuv`。

- 有 `tag` 时，脚本比较同一命名前缀下的版本，包含预发布版本。版本比较采用 packaging 规则，数字段的 `_` 和 `-` 转为 `.`；`v` 前缀可省略。无法解析的当前 tag 报错，无法解析的候选 tag 不参与比较。
- 仅有 `commit` 或 `revision` 时，脚本读取 `branch`；缺失时检查远端默认分支，并在输出中标明假设。当前 commit 是分支 HEAD 的祖先时，脚本才报告更新。历史分叉或重写报错。
- 本地源码及非 GitHub/GitLab 仓库显式输出 `SKIP`。默认识别 github.com 和 gitlab.com；自建 GitLab 使用 `--gitlab-host <host>`。

脚本只读取配方和远端引用。`UPDATE` 表示有更新，`OK` 表示当前引用无更新，`ERROR` 表示检查失败。单项失败不阻断其他项；存在 `ERROR` 时退出码为 1，否则为 0。`--timeout <seconds>` 设置每次 Git 或 API 请求的超时，默认为 30 秒。

## 添加依赖

1. 在 `third-party/<name>` 新建 `conanfile.py` 与 `conandata.yml`，使用下文模板；补丁放在 `patches/`，随配方维护的本地源码放在 `src/`。
2. 在 `conandata.yml` 声明源码、版本对应的 patch、传递依赖、上游 CMake 选项和打包元数据。关闭上游 shared library、test、benchmark、example 等不需要的目标；实际代码替换了平台分配器调用时，按下文注入 mimalloc package 和 target。
3. YSM 直接使用的依赖加入根 `conanfile.py` 的 `requirements()`；测试依赖放入 `build_requirements()` 的 `test_requires()`。仅由其他依赖使用的包放在消费者配方的 `requirements` 中。
4. YSM 直接消费的 target 在 `third-party/import.cmake` 中导入，再链接到消费模块；仅供其他依赖内部使用的包不必在主项目重复导入。具体写法见下文。
5. 更新 `THIRD_PARTY_LICENSES.md`，重新 `setup` 并构建验证。新增目录会被 bootstrap 自动发现，无需维护导出清单。

## 配方与完整数据示例

### 静态库

以下保存为 `third-party/example/conanfile.py`；包名、版本、类名和许可证须替换为实际值。`python_requires` 和 `python_requires_extend` 保持原样；`settings`、`exports_sources` 和 fPIC 配置保留。上游构建产物必须确实为静态库，`package_type` 本身不会关闭上游 shared 选项。

```python
from conan import ConanFile


class ExampleConan(ConanFile):
    python_requires = "ysm_dependency_base/0.1@ysm/stable"
    python_requires_extend = "ysm_dependency_base.YsmDependencyBaseConan"

    name = "example"
    version = "1.0.0"
    license = "MIT"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "patches/*", "src/*"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}
```

对应的完整 `conandata.yml` 示例：假定上游有可安装的 CMake 静态库，实际库名为 `example`，头文件安装到 `include`，且 patch 已将实际代码中的平台分配器调用替换为 mimalloc。替换 URL、tag、版本、选项和 patch 文件；`EXAMPLE_*` 仅代表假设的上游选项，不是公共配方预定义功能。

```yaml
sources:
  "1.0.0":
    url: https://example.com/upstream/example.git
    tag: v1.0.0
patches:
  "1.0.0":
    - patch_file: patches/example-mimalloc.patch
requirements:
  - mimalloc/3.6.0@ysm/stable
build:
  global_link_targets:
    - mimalloc-static
  options:
    BUILD_SHARED_LIBS: false
    EXAMPLE_BUILD_TESTS: false
    EXAMPLE_BUILD_BENCHMARKS: false
    EXAMPLE_BUILD_EXAMPLES: false
  install: true
package:
  cmake_file_name: Example
  cmake_target_name: Example::example
  libs: [example]
  includedirs: [include]
```

没有 patch 或传递依赖时可删除相应段；未替换平台分配器调用的库，不添加示例中的 mimalloc patch、`requirements` 中的 mimalloc 引用或 `global_link_targets` 中的 `mimalloc-static`，保留其他实际需要的补丁和依赖。`libs` 是安装后真实静态库的链接名，如 `libexample.a` / `example.lib` 对应 `example`，不是上游 CMake target 名。`cmake_target_name` 是本配方导出的消费 target，可自行命名；Conan 不会自动复刻上游 target 的全部接口属性。

### 纯头文件库与本地源码

纯头文件库按“声明包类型 → 选择源码来源 → 确定 include 布局 → 导出消费 target”接入，不要求上游有 CMake，也无需新增 `CMakeLists.txt`。

**声明包类型。** 沿用本页 Python 模板，将 `package_type` 改成 `"header-library"`，删除 `options` 和 `default_options`，保留 `settings`、`exports_sources` 和公共配方继承。公共配方因此跳过 CMake generate、build、install；`build` 整段和 `package.libs` 均可省略，不需要用 `build.install: false` 关闭构建。需要生成头文件的库必须另行实现生成步骤，不能直接套用仅复制现成头文件的路径。

**选择源码来源。** 二选一：

- Git 来源：在 `sources.<version>` 填写 `url` 与源码引用，按下文规则优先使用 `tag`；需要修改头文件时按本页补丁流程登记 patches。
- 随包源码：填写 `sources.<version>: {local: true}`，把头文件及其子目录放到配方的 `src/` 中，由 `exports_sources` 导出。此路径不执行 Git clone、`extra_git` 或 patches，修改直接维护在随包源码中。

**确定 include 布局。** 先确定消费端应写的 `#include`，再填写 `package.manual_headers`。每项从源码根目录下的 `src` 复制 `pattern` 匹配的文件到包内 `dst`，保留文件相对于该 `src` 的路径。`src` 为 `.` 表示源码根目录；Git 模式对应检出根目录，本地模式对应配方的 `src/`。

| 期望接口 | 源码中的文件 | 复制配置 | 包内文件与消费写法 |
| --- | --- | --- | --- |
| 保留库名前缀 | `include/example/api.hpp` | `src: include`、`dst: include`、`pattern: "*.hpp"` | `include/example/api.hpp`；`#include <example/api.hpp>`。 |
| 去掉上游目录前缀 | `include/example/api.hpp` | `src: include/example`、`dst: include`、`pattern: "*.hpp"` | `include/api.hpp`；`#include <api.hpp>`。 |
| 保留整套平台目录 | `api.h`、`windows/platform.h`、`posix/platform.h` | `src: .`、`dst: include`、`pattern: "*.h"` | 保留三个文件的相对布局，再按下文选择平台 include 目录。 |

`pattern` 必须覆盖被递归包含的全部文件；库混用 `.h`、`.hpp`、`.inl` 或无扩展名文件时，添加多条复制规则，或在只含必要头文件的目录使用 `"*"`。调整目录前缀时同时核验头文件内部的 include，不能只让消费端入口可见。

下面是保留库名前缀的完整 `conandata.yml`；替换版本、URL 和 tag 后即可配合上述 Python 配方使用：

```yaml
sources:
  "1.0.0":
    url: https://example.com/upstream/example.git
    tag: v1.0.0
package:
  cmake_target_name: Example::example
  manual_headers:
    - pattern: "*.hpp"
      src: include
      dst: include
```

普通布局使用默认 `package.includedirs: [include]` 即可。若公共头通过同名 `platform.h` 引入平台定义，将各平台头全部复制进包，再在 `package.platform_includedirs` 中选择目录，不把当前构建主机的那份头文件单独打包。纯头文件包清空 package ID 设置，包内容应能供不同目标消费。

例如前表的平台目录布局中，假定 Windows 使用 `windows/`，Linux、Android、macOS 使用 `posix/`，在已有 `package` 下加入：

```yaml
package:
  platform_includedirs:
    Windows: [include, include/windows]
    Linux: [include, include/posix]
    Android: [include, include/posix]
    Macos: [include, include/posix]
```

这些目录名只是示例，必须与实际复制后的布局一致；平台键使用 Conan OS 名。每个平台列表都要包含公共目录和所需的平台目录，不能同时暴露多个同名平台头。此映射只选择搜索路径，不负责复制文件。

**导出消费 target。** 设置必选的 `package.cmake_target_name`；可选的 `cmake_file_name` 省略时使用 Python 配方的 `name`。上面的完整示例对应 Conan 引用 `example/1.0.0@ysm/stable`、CMake 包名 `example`、target `Example::example`。在根配方注册该引用，在 `third-party/import.cmake` 添加 `ysm_import(example Example::example)`，再把 `Example::example` 链接到消费模块；具体 Python/CMake 写法见本页“主项目注册与链接”。即使不产生库文件，也通过 target 传播 include、编译定义及必要的接口依赖，不手工添加头文件路径。

本地源码方式也可用于静态库，但静态库仍需在源码根目录或 `build.subdir` 指定的目录内提供 `CMakeLists.txt`，不能套用纯头文件库跳过构建的行为。

## conandata 字段契约

以下覆盖公共配方读取的字段；必选性以不覆写对应阶段为前提。“条件必选”只在所述路径启用时要求填写。版本键始终用字符串，并与 Python 配方的 `version` 一致。可选段可省略或使用相应的 `{}` / `[]`，不要留下值为 YAML `null` 的空段。

### 源码、补丁与依赖

Git 源码引用优先使用所需版本的 tag；没有合适 tag 时，再在 `commit` 字段填写完整 commit revision。主仓库与 `extra_git` 均遵循此顺序。

| 字段 | 必选性 | 默认与含义 |
| --- | --- | --- |
| `sources`、`sources.<version>` | 必选 | 定义当前版本的 Git 来源或 `local: true`。 |
| `sources.<version>.local` | 可选 | 默认 `false`；为 `true` 时使用随配方导出的 `src/`。 |
| `sources.<version>.url` | 条件必选 | 非 local 模式必填 Git URL。 |
| `sources.<version>.tag` | 条件必选 | 非 local 模式与 commit 至少一个；存在非空 tag 时优先按 tag clone，不额外校验 commit。 |
| `sources.<version>.commit` | 条件必选 | 非 local 且无合适 tag 时必填完整 commit revision，并省略 tag；公共配方按该 revision 获取源码。 |
| `sources.<version>.extra_git` | 可选 | 默认 `[]`；主仓库 clone 后再获取的仓库列表。每项的 `url`、`tag`、`commit` 规则同上，另须 `target` 指定源码根目录下的目标子目录。 |
| `patches`、`patches.<version>` | 可选 | 当前版本默认无补丁；列表按顺序应用于取回的源码。 |
| `patches.<version>[].patch_file` | 条件必选 | 本文使用文件补丁，每项必填；路径相对于配方导出目录，例如 `patches/example.patch`。 |
| `patches.<version>[].base_path` | 可选 | 缺省在源码根目录应用；填写时相对于源码根目录，适用于 `extra_git` 子仓库等。 |
| `patches.<version>[].strip` / `fuzz` | 可选 | 默认 `0` / `false`；分别控制额外剥离路径层数和模糊匹配。普通 Git diff 保持默认值。 |
| `patches.<version>[].patch_description` / `patch_type` | 可选 | 仅用于补丁日志；文件补丁默认描述为文件路径、类型为 `file`。 |
| `requirements` | 可选 | 默认 `[]`；每项为引用字符串，或下述对象。所引用的项目配方也必须已由 setup 导出。 |
| `requirements[].ref` | 条件必选 | 使用对象形式时必填，例如 `mimalloc/3.6.0@ysm/stable`。 |
| `requirements[].skip_os` | 可选 | 对象形式的排除平台列表，默认 `[]`；当前目标命中时不添加该依赖。 |

补丁项交给 Conan 的 `apply_conandata_patches` 处理，补丁中的 `a/`、`b/` 前缀保持 Git diff 默认格式。Conan 也接受可选的 `patch_string` 代替文件，或不分版本的 patches 列表；本项目统一使用版本映射与 Git patch 文件。`patch_user` 不会由公共配方自动执行，其他补丁元数据不会替代实际补丁内容。

### 构建配置

`build` 整段可选，默认 `{}`，仅非纯头文件包执行这些配置。省略不意味着上游默认满足静态构建和分配器要求。

| 字段 | 必选性 | 默认与含义 |
| --- | --- | --- |
| `build.subdir` | 可选 | 默认源码根目录；指定上游 CMake 入口的相对子目录。 |
| `build.options` | 可选 | 默认 `{}`；键为上游 CMake 变量名，值写入 Conan toolchain variables。它不是 Conan 的 `options` 声明。 |
| `build.cache_variables` | 可选 | 默认 `{}`；键值写入 CMake cache variables。 |
| `build.global_link_targets` | 可选；静态库实际替换平台分配器调用时须加入 mimalloc target | 默认 `[]`；向依赖工程后续创建的 target 注入链接要求。填写实际导出的 target，如 `mimalloc-static`，不能填写 Conan 引用。 |
| `build.install` | 可选 | 默认 `true`，执行上游 install；`false` 则递归复制构建目录内的 `*.lib` / `*.a` 到包的 `lib/`，不保留子目录。 |
| `build.copy_built_libraries` | 可选 | 默认 `false`；install 为 true 时额外执行上述库复制，弥补上游 install 漏装静态库。install 为 false 时无需再设此项。 |

`options` / `cache_variables` 的值可为布尔、数值、字符串，或按目标 OS 分支的映射；字符串中的 `{source_folder}` 替换为源码根目录的绝对路径。平台选择先读 `user.target:os`，未设置时取 Conan `settings.os`，再转成小写；`skip_os` 使用同一选择规则。官方 profiles 常用 `windows`、`linux`、`android`、`macos`；未覆盖 `user.target:os` 时 Conan 的 `Macos` 对应 `macos`。

```yaml
build:
  options:
    EXAMPLE_USE_ASM:
      windows: false
      default: true
    EXAMPLE_CONFIG: "{source_folder}/config.h"
```

分支先匹配当前平台，再取 `default`。`cache_variables` 两者都缺失时不写该变量；`options` 应给出 `default` 或覆盖全部使用平台，避免把未匹配的空值传入 toolchain。此语法不适用于 `global_link_targets` 等其他字段。

### Android 第二身份

Android 同时保留 native 工具链的原生身份路径和显式第二身份路径，两者都应兼容。第二身份是刻意的构建设计，不是历史遗留问题：它以通用 Linux 交叉编译路径作为基础，由显式编译器、目标 triple、sysroot 和 runtime 决定产物，再单独选择 Android 特化行为。此约定只适用于第二身份路径，不要求 native 工具链把原生 Android 身份改成 Linux。

|  | Native 工具链路径 | 显式第二身份路径 |
| --- | --- | --- |
| Conan | `settings.os = Android`，可不设置 `user.target:os`；公共配方的 `_target_os` 回退到 settings，得到 `android`。 | `settings.os = Linux`、`user.target:os = android`；`_target_os` 优先使用第二身份。 |
| CMake | 未显式设置 `TARGET_OS` 时，`CMAKE_SYSTEM_NAME = Android` 或 CMake 变量 `ANDROID` 为真均识别为 Android，优先于通用 Linux 判断。 | `CMAKE_SYSTEM_NAME = Linux`、`TARGET_OS = ANDROID`；项目平台分派保留显式身份，不用基础系统名覆盖。 |

两条路径最终都选择 Android 平台配置并定义 `YSM_ANDROID`；公共配方的 `build.options`、`build.cache_variables` 和 `requirements.skip_os` 都按解析后的 `_target_os` 选择。前提是 Conan 与 CMake 已由当前工具链正确配置目标；这不是让 `bootstrap build native` 自动从桌面环境切换到 Android。

第二身份由构建配置提供，依赖使用时不根据构建主机推断，也不为触发上游分支而改写这条路径的基础身份。编译器侧的 Android 宏与 CMake 变量是不同层次：即使编译选项定义了 `ANDROID=1`，也不会让 CMake 的 `if(ANDROID)` 自动成立。

**依赖只要对 Android 构建有特化，就必须通过 Git patch 让相关判断识别第二身份。** 保留上游原生 Android 判断，并增加 `TARGET_OS` 判断；当前 cpu_features 对 `ndk_compat` 的处理可归纳为：

```diff
-if(ANDROID)
+if(ANDROID OR "${TARGET_OS}" STREQUAL "ANDROID")
 add_subdirectory(ndk_compat)
 endif()
```

此写法同时适用于原生 Android CMake 环境和本项目的第二身份路径。补丁按本页 Git patch 流程生成并登记到 `patches.<version>`。检查全部 Android 特化入口，包括源码选择、兼容子库、配置探测、编译定义和系统链接库；存在 Linux fallback 时先判断 Android，避免被基础 Linux 身份提前匹配。

若 patch 启用了额外兼容库或头文件，打包元数据也必须完整导出它们。cpu_features 在 `ndk_compat` 产物存在时增加其 include 目录，并把该库纳入链接列表；不能只启用上游子目录而漏掉消费者所需的头文件或库。

Python 配方需要同时兼容两条 Android 路径时使用继承的 `self._target_os == "android"`，不要只判断 `self.settings.os == "Android"`。注意 `package.platform_includedirs` 仍按原始 `settings.os` 选择：native Android 取 `Android` 项，Linux 基础的第二身份路径取 `Linux` 项。若 Android 需要不同头文件目录，在该包的 `package_info()` 中调用 `super().package_info()` 后按 `_target_os` 覆写，例如：

```python
def package_info(self):
    super().package_info()
    if self._target_os == "android":
        self.cpp_info.includedirs = ["include", "include/android"]
```

示例目录必须由 `manual_headers` 或安装规则实际放入包内。验证至少覆盖原生 Android、Linux 基础身份 + Android 第二身份、普通 Linux 三条路径，确认前两者启用 Android 特化、后者不误启用。

### 打包与消费 target

`package` 必选；选择单 target 或非空 `components` 两种形式之一。库默认位于包内 `lib/`，include 路径相对于包根目录。

| 字段 | 必选性 | 默认与含义 |
| --- | --- | --- |
| `package.cmake_file_name` | 可选 | 默认 Python 配方的 `name`；是 `ysm_import` / `find_package` 使用的包名。 |
| `package.cmake_target_name` | 条件必选 | 没有非空 components 时必填，包含纯头文件包；使用 components 时忽略此字段，聚合 target 名由 `cmake_file_name`（缺省为包名）决定。 |
| `package.libs` | 可选 | 单 target 静态库省略时自动收集包内库；显式 `[]` 表示不链接库。纯头文件包忽略此字段。多库有链接顺序要求时显式列出或使用 components。 |
| `package.includedirs` | 可选 | 单 target 默认 `[include]`；仅非空列表会覆盖，`[]` 不能用于清空默认值。 |
| `package.platform_includedirs` | 可选 | 默认 `{}`；非空时按原始 Conan OS 名（`Windows`、`Linux`、`Android`、`Macos`）选择并覆盖 includedirs，未命中回退 `[include]`。与构建选项的小写平台键不同。 |
| `package.manual_headers` | 可选；纯头文件模板必填 | 默认 `[]`；从源码根目录复制文件到包。无上游头文件 install 规则时也需填写。 |
| `package.manual_build_headers` | 可选 | 默认 `[]`；从构建目录复制生成的头文件到包。 |
| 上述两类列表的 `pattern`、`src`、`dst` | 条件必选 | 每项三者均必填：匹配模式、相对于来源根目录的目录、相对于包根目录的目标目录。保留来源目录内的相对路径。 |
| `package.components` | 可选 | 默认无组件；非空映射将包拆为多个消费 target，键是 Conan 组件名。此模式下顶层 libs 不生效，各组件使用自己的 includedirs。 |
| `package.components.<name>.target` | 条件必选 | 每个组件必填导出的 CMake target 名。 |
| `package.components.<name>.libs` | 可选 | 默认 `[]`，不会自动收集库；需要二进制库的组件必须填写实际链接名。 |
| `package.components.<name>.includedirs` | 可选 | 默认 `[include]`，相对于包根目录。 |

没有 install 规则时设 `build.install: false`，用 `manual_headers` / `manual_build_headers` 补齐头文件。自动库复制会包含构建目录中所有匹配库，必须关闭无关 target，并避免不同子目录下的同名库被合并覆盖。

## 主项目注册与链接

对上面的静态库示例，在根 `conanfile.py` 已有 `requirements()` 方法中**追加**以下行，保留其他依赖：

```python
self.requires("example/1.0.0@ysm/stable")
```

在 `third-party/import.cmake` 追加导入，再在消费模块的 `CMakeLists.txt` 中追加链接（以下假定消费者为 `ysm-codec`）：

```cmake
# third-party/import.cmake
ysm_import(Example Example::example)

# modules/codec/CMakeLists.txt
target_link_libraries(ysm-codec PRIVATE Example::example)
```

仅实现使用时选 `PRIVATE`；依赖类型出现在 public header 时选 `PUBLIC`，接口 target 使用 `INTERFACE`。不要手工拼 include / library 路径。这里的 `Example` 来自 `cmake_file_name`，`Example::example` 来自 `cmake_target_name`，与 Conan 的 `example/1.0.0@ysm/stable` 是三个不同标识。

测试依赖在根配方已有 `build_requirements()` 中追加 `self.test_requires("example/1.0.0@ysm/stable")`，在 `third-party/import.cmake` 已有 `if(BUILD_TESTING)` 块内导入，仅链接测试 target；benchmark 依赖的导入使用 `if(YSM_BUILD_BENCHMARKS)`。仅由第三方消费者使用的传递依赖直接加入该消费者的 `conandata.yml requirements`；其 CMake 使用的包名与 target 名必须和依赖导出的名字一致，否则调整导出名或 patch 上游 CMake。

## 非 CMake 依赖

需要编译的依赖库若不基于 CMake，必须额外编写 `CMakeLists.txt`，通过 Git patch 添加到上游源码，并在 `patches.<version>` 登记。纯头文件库按本页“纯头文件库与本地源码”方式直接打包，不受此要求约束。需要编译的库仍统一使用 CMake 构建，不通过覆写构建阶段改用上游的 Meson、Autotools 等构建系统。

新增 CMake 工程只需包含本项目需要引用的 target 及其必要源码、生成步骤和内部依赖；无需迁移上游完整构建系统，不纳入无关的 benchmark、test、CLI、example 等代码。显式列出所需源码，避免将这些目录一并编入库。

在 CMake 中声明实际使用的 C/C++ 语言、静态库 target、include 目录、编译定义及链接依赖，保留所需的平台选择与配置头生成。编译器、runtime 和共享构建参数由项目工具链与 hook 提供；实际替换平台分配器调用时，再按本页规则注入 mimalloc。

默认将 `CMakeLists.txt` 添加到源码根目录；放在子目录时设置 `build.subdir`。若提供安装规则，将静态库和头文件安装到 `lib/`、`include/`；否则设置 `build.install: false` 并补齐 `package.manual_headers` / `manual_build_headers`。`package.libs` 必须对应实际输出的静态库名，消费 target 仍由 package 元数据导出。生成 Git patch 时须将新增的 `CMakeLists.txt` 纳入 diff，操作方式见下文补丁流程。

## 公共配方的扩展边界

仅有消费 target 元数据差异时覆写 `package_info()`，不复制整份公共配方。

YAML 的 components 只支持表中三个字段，**不读取**组件 `requires`、`defines`、`system_libs`、frameworks 或额外编译/链接选项。消费者需要这些信息时，在包的 Python 类中补充；调用 `super().package_info()` 保留 YAML 已生成的元数据。例如两个静态库组件可写：

```yaml
package:
  cmake_file_name: Example
  components:
    core:
      target: Example::core
      libs: [example_core]
    adapter:
      target: Example::adapter
      libs: [example_adapter]
```

```python
def package_info(self):
    super().package_info()
    self.cpp_info.components["adapter"].requires = ["core"]
    self.cpp_info.components["core"].defines = ["EXAMPLE_STATIC=1"]
```

示例中的宏必须替换为上游要求；不要无依据添加。单 target 的接口属性写在 `self.cpp_info`；系统库写入 `system_libs`，Apple frameworks 写入 `frameworks`。实际目标平台判断使用 `self._target_os`，尤其需按本页“Android 第二身份”规则区分 Android 与 Linux。组件依赖用 Conan 组件名，包内写 `core`，跨包写 `dependency_name::component_name`，不是 CMake target 名。主项目需要哪些组件，就通过 `ysm_import(Example Example::adapter)` 导入检查并链接对应 target。

当依赖的 public header 或接口要求继续向下游传播另一依赖的头文件 / 库时，YAML 的字符串引用不能表达 Conan traits；在该包类中覆写 `requirements()`，用 `self.requires("dependency/1.0.0@ysm/stable", transitive_headers=True, transitive_libs=True)` 按实际需要指定。若要保留其他 YAML requirements，先调用 `super().requirements()`，并从 YAML 删除这条已手写的引用，避免重复声明。

## 注入 mimalloc package 与 target

只有进入生产制品的依赖需要接入 mimalloc。仅供测试或性能基准使用的依赖（如 Google Benchmark）不必添加分配器 patch、mimalloc requirement 或 target 注入。依赖同时供生产和测试使用时，按生产依赖处理。

对生产依赖，实际代码已将平台分配器调用替换为 `mi_*`（显式替换或通过 `mimalloc-override.h` 映射）时，才需要注入 mimalloc package 和 target。未发生这种替换时，不因它是 YSM 依赖、使用 CMake 或已有其他源码 patch 就添加 mimalloc。

满足上述条件的静态库，在**该依赖自身**的 `conandata.yml` 中向 `requirements` 加入 `mimalloc/3.6.0@ysm/stable`，向 `build.global_link_targets` 加入 `mimalloc-static`；完整写法见本页静态库示例。合并到已有列表并保留其他依赖和选项，mimalloc 版本与根项目配方保持一致。

`requirements` 将 mimalloc 纳入 Conan 依赖图；`build.global_link_targets` 经公共配方和 CMake hook 把 `mimalloc-static` 注入该依赖工程，使后续 target 获得其头文件与链接要求。无需为每个上游 CMake target 单独添加 mimalloc。

此注入只提供编译和链接能力，分配器调用仍须通过 patch 改造。当前 mimalloc 配方关闭 `MI_OVERRIDE`、`MI_WIN_REDIRECT` 和 macOS interpose/zone 接管，不依赖进程级替换来接管宿主 JVM 的分配器。纯头文件包不会运行公共配方的 CMake 注入；其头文件若调用 `mi_*`，按本页 traits 扩展方式声明 mimalloc 的 `transitive_headers=True` 和 `transitive_libs=True`，向消费 target 传播 include 与链接要求。

## 通过 Git patch 替换分配器

在与 `sources` 对齐的上游 Git 检出中修改，生成 patch 保存到配方的 `patches` 目录，并在 `patches.<version>` 中登记 `patch_file`；完整写法见本页静态库示例。

在上游检出中使用 `git diff --binary --output=<patch-path>` 生成补丁；新增文件先用 `git add -N <file>` 纳入 diff。可在相同基线的干净检出中以 `git apply --check <patch-path>` 检查。`exports_sources` 必须包含 patch。多份 patch 应按配方中的顺序逐份检查并应用，确保后一份的基线含前一份修改。不要直接修改 Conan cache 内的源码作为最终交付。本地随包源码应直接维护，不经过 patch 阶段。

升级源码后，应从新基线逐份重新生成 patch，并通过 Conan 的源码阶段验证。`git apply --check` 可接受行号偏移，通过检查不能保证 Conan 的补丁器能应用。

### 头文件

在当前文件最后一个 `#include` 后添加 `#include <mimalloc.h>`，逐个替换当前文件中所有应由 mimalloc 管理的分配、重分配与释放调用，例如 `malloc` → `mi_malloc`、`calloc` → `mi_calloc`、`realloc` → `mi_realloc`、`free` → `mi_free`。同时检查 aligned allocator、分配器包装宏和函数指针等间接入口，按实际 API 签名适配。

头文件不用 `mimalloc-override.h`，避免替换宏传播到其他文件；`std::malloc` 等限定名应改为 `mi_malloc`，不能保留错误的 `std::mi_malloc`。

### 源文件

在当前文件最后一个 `#include` 后添加 `#include <mimalloc-override.h>`，由宏将该文件后续的平台分配器调用映射到 `mi_*`。插入位置必须覆盖相关平台分支，同时不能污染随后包含的系统或第三方声明。

如果 override 宏误替换同名成员、限定名或其他符号导致编译失败，降级为头文件方式：移除 `mimalloc-override.h`，在最后一个 include 后改加 `mimalloc.h`，逐个显式替换该文件的分配器调用。未被 override 覆盖的平台分配 API 也应显式适配。

### 必须保留的分配边界

- 平台提供的非内存分配功能若隐式分配内存，且其契约要求标准 `free`，必须继续用标准 `free` 释放，不能改成 `mi_free`。同一源文件同时包含这类路径和 mimalloc 路径时，采用显式替换方式，确保标准 `free` 不受 override 宏影响；平台要求专用释放函数的仍使用对应函数。
- 沿对象所有权核对分配、重分配和释放两端，包括回调与跨文件释放。不能只替换分配端或释放端，也不能让 mimalloc 内存回到仍使用平台 allocator 的路径。
- 依赖若重写全局 `operator new` / `operator delete` 接入自己的分配器，优先通过 `build.options` 中对应的上游 CMake option 关闭；没有可用选项时以 Git patch 移除，避免争夺最终链接产物的全局分配入口。
- 不参与产品构建的 test、benchmark、example 等代码可不替换；一旦被编入生产 target，就必须纳入分配器审查。

## 导出、构建与验证

以下采用仓库内[构建指南](build_cn.md)的系统环境流程，命令均在 C++ 项目根目录执行。准备 Git 2.49+、Conan 2.28+、CMake 3.29+、Ninja、NASM 和可访问源码地址的网络，确保 PATH 中没有 Yasm。Windows 使用启用 C++ 与 CMake 组件的 Visual Studio 2022+，MSVC 构建前初始化 vcvars；GNU/Linux 使用支持 C++23 的 build-essential，Android 使用支持 C++23 的 NDK，macOS 使用支持 C++23 的 Xcode。

Windows PowerShell：

```powershell
.\bootstrap.cmd setup
.\bootstrap.cmd build native --deps-only
.\bootstrap.cmd build native
```

Linux / macOS Shell 使用同一组参数：

```sh
bash ./bootstrap.sh setup
bash ./bootstrap.sh build native --deps-only
bash ./bootstrap.sh build native
```

`native` 使用当前主机环境，不会因安装了 Android NDK 等交叉工具就自动切换目标平台。非官方目标的环境约束与适配检查见[非官方平台适配](platform-porting_cn.md)。

每条命令成功后再进入下一步。`setup` 只导出配方；`--deps-only` 阶段会构建缺失依赖并生成主项目配置，仅在 install 成功后清理依赖构建目录，可用于单独定位依赖问题，也可省略。profile 探测、install 或清理失败均返回非零状态，install 失败保留构建目录供排查。最后的完整 build 才会构建 `ysm-lib-jni` 并验证最终链接。默认 Release，两个 build 命令均追加 `--debug` 可切换 Debug。

默认 Conan 缓存是 `conan/cache`。需要隔离缓存时给所有 setup/build 命令传相同的 `--cache <cache-path>`；bootstrap 禁用 Conan remote，遗漏的传递依赖必须补充本地配方并 setup。上述流程的主项目输出目录为 `build/native-release`，Debug 为 `build/native-debug`，Conan 生成文件位于其 `generators/` 下，不直接修改这些生成文件。

配方、数据或 patch 改动后重新 setup，再 build；共享 hook/profile 的二进制设置变化则还需递增 `conan/profile-override` 中的 `user.ysm:rev`，避免复用旧二进制。更换系统编译器、SDK 或 runtime 后核对 Conan settings，使用新的隔离缓存验证，不能假定系统变化都自动计入缓存键。当前流程不显式生成或传入 lockfile，版本与源码引用由本页的配方字段固定。

完整 build 不自动运行测试。对能在当前主机执行的目标，启用测试并构建、运行相关用例；以下承接 PowerShell 示例，`<test-regex>` 替换为相关 CTest 用例匹配式：

```powershell
cmake -S . -B build/native-release -DBUILD_TESTING=ON
cmake --build build/native-release --target ysm-test
ctest --test-dir build/native-release -R "<test-regex>" --output-on-failure --no-tests=error
```

这里复用 bootstrap 已配置的构建目录，使用同一环境中的 CMake / CTest；Shell 命令相同，Debug 时改用对应构建目录。交叉目标需在匹配的运行环境执行测试，不能把“编译通过”当成运行验证。

验收时确认依赖安装包包含所需静态库和头文件，消费模块能仅凭导出的 target 完成编译链接；运行覆盖实际分配、失败清理、重分配和释放路径的最小相关测试。allocator 或 public header 改动需核验所有受支持平台的相关分支，明确记录未验证部分。检索剩余 `malloc` / `calloc` / `realloc` / `free` 时，区分宏已接管的调用、平台释放例外和未参与构建的代码，不能仅靠文本零命中判断改造完成。

### 分配器符号检查

新增、升级生产依赖或修改其分配器配置、patch 后，维护者应使用工具链中的 `llvm-nm` 核验内存分配器引用。检查范围应覆盖生产 target 消费的依赖静态库。纯头文件依赖应检查生产 target 中实例化其代码的对象文件。仅供测试或性能基准使用的依赖不要求此项检查。以下命令输出符号及所属对象文件；将路径替换为实际的 `.a`、`.lib` 或对象文件：

```sh
llvm-nm --print-file-name --demangle "<artifact-path>"
```

检查 `malloc`、`calloc`、`realloc`、`free`、对齐分配 API 及其平台变体的未定义引用。核对替换路径是否引用预期的 `mi_*` 符号，并检查依赖是否定义了自己的全局 `operator new` / `operator delete`。对残留的平台分配器引用，应按对象文件回查源码，确认分配与释放配对，并说明保留依据；平台释放例外按本页“必须保留的分配边界”判断。

`llvm-nm` 失败或产物缺少可检查的符号时，应记录为未验证。符号检查不能证明间接调用和运行时所有权正确，仍需执行上述分配与释放测试。
