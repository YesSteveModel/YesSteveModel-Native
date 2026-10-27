# 官方构建环境

此为官方构建使用的完整工具链和环境，可基于此复现官方构建。

<sub>*常规开发不需要也不推荐手动复现。*</sub>

## 工具链

| 组件 | 版本     |
| --- |--------|
| LLVM | [23.1.4 定制](https://github.com/TomatoPuddin/llvm-project/releases/tag/tomato-23.1.4) |
| CMake | 4.4.4  |
| Ninja | 1.13.2 |
| NASM | 3.01   |
| Conan | 2.33.0 |

- Windows target 使用 clang-cl 编译器前端；
- 所有 target 均使用 lld 链接器。

## 目标环境

| 目标 | 内核/平台基线                                    | C 标准库 | STL + runtime                             | SDK / sysroot |
| --- |--------------------------------------------| --- |-------------------------------------------| --- |
| Windows x86_64 | Windows 10.0.26100.0 API Surface           | UCRT 10.0.26100.0 | libc++ ; VC Runtime 14.44        | Windows SDK 10.0.26100.0 |
| Linux x86_64 | Linux 3.13.9 Kernel Headers                         | glibc 2.19 | libc++ ; libc++abi ; libunwind     | Ubuntu 14.04 sysroot |
| Android arm64 | Android API 28       | Bionic | libc++ ; libc++abi ; libunwind (with NDK patches) | Android NDK r30 sysroot |
| macOS arm64 | Darwin 23.5.0；macOS 14.5 deployment target | libSystem in macOS SDK | libc++ ; libc++abi ; libunwind     | macOS 14.5 SDK |

- macOS target 链接自构建的 `libc++_static.a`，因此不会使用 macOS SDK 提供的动态 libc++ runtime。
- Linux x86_64 和 macOS arm64 target 使用的 libc++ runtime 均启用了 unstable ABI v2.
