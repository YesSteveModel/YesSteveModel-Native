# Build Guide

The following is the general build flow for contributors. It is intended for local development, testing, and ordinary contribution work, and may not exactly match the maintainer release environment.

The project has a complex native dependency stack and supports multiple target platform environments. The maintainers try to keep this flow usable, but cannot guarantee it will work smoothly on every machine or every build environment combination.

If you run into trouble, need help matching the expected environment, or need to investigate a release-build difference, ask for help in the contributor chat group.

## 0. Prerequisites

- Git >= 2.49 (important)
- Network access that can reach GitHub reliably
- Windows only: Visual Studio 2022+ (with the C++ workload and CMake components enabled)
- GNU/Linux only: build-essential (with C++23 support)
- Android only: Android NDK (with C++23 support)
- macOS only: Xcode (with C++23 support)
- CMake >= 3.29 + Ninja (Visual Studio and NDK already include them)
- NASM
- Conan >= 2.28
- Make sure Yasm is not in `PATH`.

## 1. Index Dependencies

Run this from the project root:

### Windows Example

```bash
.\bootstrap.cmd setup
```

<sub>*If you are building with Visual Studio's MSVC toolchain, you must first run the vcvars script to set up the command-line environment.*</sub>

### Linux Example

```bash
./bootstrap.sh setup
```

This indexes the dependency recipes into the repository-local Conan cache at `conan/cache`. You usually only need to run it for the first build or after dependency changes.

## 2. Build the Project

Run this from the project root:

### Windows Example

```bash
.\bootstrap.cmd build native
```

### Linux Example

```bash
./bootstrap.sh build native
```

After this step, you can open and index the project with a CMake-capable IDE.

For the toolchain versions and target environments used in official builds, see [Official Build Environment (Chinese only)](official-build-environment_cn.md).
