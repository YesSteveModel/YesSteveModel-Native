# 构建指南

以下是面向贡献者的通用构建流程，主要用于本地开发、测试和日常贡献，可能与维护者所使用的发布环境不完全一致。

本项目的依赖比较杂，且需要适配多种目标平台。维护者会尽力保证这套构建流程可用，但无法确保在每台机器、每种构建环境的组合下都能成功构建。

如果在构建过程中遇到问题、需要协助对齐预期的构建环境，或想排查发布版与本地构建的差异，请在贡献者交流群中寻求帮助。

## 0. 前置要求

- Git >= 2.49（很重要）
- 良好的网络环境（能流畅访问 GitHub)
- 仅 Windows：Visual Studio 2022+ (启用 C++ 负载和 CMake 组件)
- 仅 GNU/Linux：build-essential（支持 C++23）
- 仅 Android：Android NDK（支持 C++23）
- 仅 macOS：XCode（支持 C++23）
- CMake >= 3.29 + Ninja （VS 和 NDK 已集成）
- NASM
- Conan >= 2.28
- 确保 `PATH` 环境变量里没有 Yasm

## 1. 索引依赖项

在项目根目录执行:

Windows 示例:

```bash
.\bootstrap.cmd setup
```

<sub>*如果使用 Visual Studio MSVC 构建，要先为命令行设置 vcvars*</sub>

Linux 示例:

```bash
./bootstrap.sh setup
```

这一步会把 recipe 索引到仓库内的 Conan 缓存 `conan/cache`，通常只有首次构建项目或依赖项发生变化时才需要执行。

## 2. 构建项目

在项目根目录执行:

Windows 示例:

```bash
.\bootstrap.cmd build native
```

Linux 示例:

```bash
./bootstrap.sh build native
```

完成后可使用支持 CMake 的 IDE 打开并索引此项目。

官方构建使用的工具链版本与目标环境见[官方构建环境](official-build-environment.md)。

## 3. 运行单元测试和性能基准

`test` 构建 `ysm-test` 并通过 CTest 运行单元测试。`benchmark` 构建并运行 `ysm-benchmark`。

```powershell
.\bootstrap.cmd test native
.\bootstrap.cmd benchmark native
```

在 Linux 或 macOS 上使用 `./bootstrap.sh` 和相同参数。这两个命令沿用 `build` 对 profile、`--toolchain`、`--cache` 和 `--debug` 的约定。交叉编译或只需生成可执行文件时，追加 `--build-only`，再按需在目标环境运行。

通过 CTest 运行时，构建配置会传入源码树的 `modules/test/data`。将 `ysm-test` 复制到目标环境后，可用 `--fixture-dir=<path>` 指定该目录在目标环境中的位置。Windows 示例假定当前目录中有测试程序和复制过来的 `data` 目录。

```powershell
.\ysm-test.exe --fixture-dir=.\data
```

直接运行时，省略该参数会跳过依赖外部样本的用例。传入目录后，所需样本缺失、无法读取或损坏会使相应用例失败。
