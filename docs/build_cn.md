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

官方构建使用的工具链版本与目标环境见[官方构建环境](official-build-environment_cn.md)。
