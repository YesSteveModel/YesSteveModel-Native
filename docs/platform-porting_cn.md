# 非官方平台适配

本页用于评估和实现官方支持范围之外的 OS、CPU 架构或 runtime 组合。官方构建环境与目标矩阵以仓库内[官方构建环境](official-build-environment_cn.md)为准；这里不预设额外工具链包、安装目录或交叉 profile 已经存在。完成一次构建不代表该平台进入官方支持范围。

## 先定义目标与成本

先明确实际运行者使用的 OS 最低版本、CPU/ISA、位宽与端序、C 库、C++ 标准库和 ABI、SDK/API 基线，以及可用的目标 JVM。区分运行构建工具的主机与加载 native 动态库的目标，确定可以执行测试的真实目标环境。

| 变化 | 主要适配成本 |
| --- | --- |
| 已有 OS/架构，更换编译器、runtime 或最低系统版本 | 检查编译/链接选项、标准库 ABI、系统符号版本与依赖兼容性；即使源代码无需改动，也需全量重建依赖和目标机回归。 |
| 已有 OS，新增 CPU 架构 | 增加平台分派、ISA/SIMD 与汇编路径，核验全部依赖的该架构后端和 JNI ABI；成本取决于是否已有可用的标量或同族实现。 |
| 新 OS | 还需处理系统 API、线程/TLS、动态库格式、链接与导出规则、JNI 平台头，以及 Java 侧的平台识别和加载。 |
| 改变位宽、端序或对象布局假设 | 额外审查指针/整数转换、对齐、原子操作、SIMD 数据布局和二进制读写；不能作为仅增加一个 CMake 分支的工作估算。 |

评估应分别给出首次适配工作量、依赖升级后的 patch 维护量、目标机测试资源和回归矩阵成本。缺少 C++23 编译器、所需标准库能力、依赖后端或可运行的 JVM 时，先确认补齐路径，再承诺支持范围。

## 1. 建立可重复的构建环境

本机适配从仓库构建指南的入口开始，在 C++ 项目根目录执行：

```powershell
.\bootstrap.cmd setup
.\bootstrap.cmd build native
```

Shell 对应 `bash ./bootstrap.sh setup` 和 `bash ./bootstrap.sh build native`。前置工具、Windows vcvars 和 PATH 要求遵循仓库构建指南。`native` 代表探测到的主机环境，不是任意新平台的别名。

交叉编译必须另外建立目标环境，不能只把 `native` 换成一个新名字：

- Conan host 上下文描述目标 OS、arch、compiler、runtime、build type；build 上下文描述能在构建主机运行的工具。目标库与代码生成工具不能混用同一侧的二进制。
- 显式指定编译器、链接器、目标 triple、SDK/sysroot 和库搜索边界，保留项目 `conan/profile-override` 的共享设置与 CMake hook。不要让目标依赖探测到主机的系统库。
- 保证 CMake 的目标系统/处理器信息与 Conan host settings 一致；项目配方还需要 `user.target:profile` 区分输出目录。配置名称不能代替真实的目标设置。
- 上游 configure 的运行探测、代码生成与测试发现必须适合交叉环境：使用可在构建主机运行的工具，或提供经过目标验证的探测结果；不得为了配置通过就把目标可执行文件当作主机程序运行。

仓库通用构建指南没有承诺上述新平台的现成交叉命令。适配交付需记录自己的环境配置和重建命令；若扩展 bootstrap，同步维护 CMD/Shell 的目标选择与主机识别，不把单机目录写入项目配置。

## 2. 补齐 CMake 平台分派

`conan/cmake/target-dispatch.cmake` 将 CMake 系统/处理器归一为 `TARGET_OS`、`TARGET_ARCH`，生成 `YSM_<OS>`、`YSM_<ARCH>` 与小写 `YSM_TARGET`，再选择 `conan/cmake/targets/<os>-<arch>.cmake`。当前显式识别 Windows、Linux、Android、Darwin/macOS 及 x64、arm64；未知值保留大写后的实际名称并警告，例如 FreeBSD/riscv64 得到 `FREEBSD` / `RISCV64` 和 `freebsd-riscv64`。这不构成通用平台支持，仍需补齐目标配置和必要的别名映射。

新目标配置需逐项确定：

- 最低 ISA 与必要架构宏，不复制其他架构的 `-march`、`-mcpu` 或 `_AMD64_` 等设置。
- 编译器前端、异常/RTTI、C++ runtime、PIC、符号可见性、线程与平台系统库。
- 动态库格式对应的导出控制、链接器参数、符号裁剪、LTO 和页对齐要求；不能将 ELF、Mach-O、PE 的选项互换。
- Debug/Release 都可配置；依赖和主项目共享必要的 ABI 设置。探测阶段与实际 target 构建的参数差异必须有明确理由，注意现有 `ysm_in_try_compile` 分支。

平台 hook 会作用于依赖构建，不能只调整主项目最终链接。共享配置变化后更新 `user.ysm:rev`，并使用隔离 Conan 缓存重建验证，防止旧平台包掩盖遗漏。

## 3. 核验整个依赖图

按[依赖接入文档](dependencies_cn.md)维护配方和补丁，并核验直接、传递及测试依赖：

- 需要编译的非 CMake 依赖按依赖文档的“非 CMake 依赖”规则补齐构建入口，核验新增 CMake target 在目标平台上的源码选择和配置头生成；纯头文件库使用该文档的直接打包路径。
- 确认上游支持目标的编译器、C/C++ runtime、汇编语法和 ISA；没有合适汇编后端时，选择可用且经过验证的通用实现，不能假设关闭 ASM 一定可行。
- 检查 `requirements.skip_os`、构建选项的平台分支、组件系统库和平台头目录。条件排除了某个包时，上游链接引用和产品所需能力也必须闭环。
- Android 的原生身份、刻意第二身份及上游特化 patch 方法遵循依赖文档的“Android 第二身份”规则；验证 native Android、Linux 基础的 Android 第二身份和普通 Linux 三条路径。
- 验证 mimalloc 的目标支持、TLS 模型、对齐分配、退出清理及现有 patch 的平台分支。显式分配与释放保持配对，平台 API 隐式分配的内存仍使用其约定释放方式。
- 全部生产依赖静态链接，核对 runtime ABI 和最终产物的系统动态依赖；不以系统预装的另一版依赖替代缺失的项目配方。

补丁能应用只说明文本基线匹配，不能证明新平台的 allocator、ABI 或指令集语义正确。

## 4. 补齐运行时与 JNI 边界

CPU 探测和 SIMD 路径必须与目标基线一致。当前 `InitCpuInfo()` 对 x64 / arm64 有专门分支，其他架构返回 `simd::Type::None`；这不保证所有消费方已有可用的标量实现。核验渲染、codec 和依赖中的 SIMD 分派、intrinsics、内联汇编及全局初始化，不能仅让平台宏通过编译。更高 ISA 的函数仍需由 runtime feature dispatch 保护。

逐项核验文件/路径、时钟、线程/TLS、CPU 拓扑、内存映射和分配等系统接口；能力探测失败时，按现有功能契约处理降级，不能改变必需功能的语义。

JNI 侧检查目标 JVM 的架构与 native 产物一致、`jni_md.h` 的平台定义和调用约定正确，并补齐动态库命名、导出列表及 Java 侧加载路径。`third-party/jni` 的平台 include 映射与 `ysm-lib-jni` 的平台链接规则都是检查点；仅生成动态库不足以验证 JVM 能加载及调用它。保持 C ABI 导出与所有权边界，不暴露 C++ 对象或跨 JNI 传播异常。

## 5. 验收与交付

| 验证层 | 必须取得的证据 |
| --- | --- |
| 配置与编译 | 隔离缓存下可重建全部依赖及主项目；Debug/Release 无错误平台宏、主机头文件或旧包混入。 |
| 链接与产物 | 架构、位宽、导出符号、runtime 与系统库依赖符合目标；较老系统基线未被无意提升。 |
| 目标机运行 | native 测试以及真实 JVM 加载/JNI 调用通过；覆盖分配/释放、失败清理、线程/TLS 与至少一条实际模型或 codec 路径。 |
| ISA 与回归 | 在最低支持 CPU 上验证启动与通用路径，高 ISA 分派有对应证据；复查受共享配置或 patch 影响的现有平台。 |

测试 target 为 `ysm-test`，启用 `BUILD_TESTING` 后单独构建与运行；完整项目 build 默认只构建 `ysm-lib-jni`。交叉编译下能发现测试用例，不代表能在构建主机执行，应在匹配目标环境运行，具体命令与配置复用方式见依赖文档的验证部分。

交付中记录环境版本、目标基线、重建与测试步骤、已验证能力和剩余限制。新增适配不自动修改官方支持矩阵；正式纳入支持时再同步仓库构建指南及相应支持说明。不能用当前机器一次成功运行，替代最低系统与最低 ISA 的验证。
