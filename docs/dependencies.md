# 依赖管理方式

本页说明第三方依赖的托管方式、准入评估标准与硬性约束，用于架构评估与选型决策。具体配方模板、`conandata.yml` 字段契约、补丁改造与构建验收步骤见[添加依赖细则](dependency-adding.md)；构建系统原理见[Bootstrap、Conan 与 CMake 构建系统](build-system.md)。

## 托管与解析机制

- **本地源码配方**：全部第三方依赖在 `third-party/<name>` 下维护独立的 Conan 2 配方（继承公共配方 `ysm_dependency_base`），由 bootstrap 自动发现并导出；构建期间禁用远端 Conan remote，不允许以系统预装库替代缺失配方。
- **版本锁定**：依赖源码引用与补丁由各配方的 `conandata.yml` 固定（优先 Git tag，无合适 tag 时使用完整 commit revision，或随配方维护的本地源码）；上游更新统一通过 `python third-party/check_updates.py` 检查。
- **目标导入与链接收窄**：直接依赖注册在根 `conanfile.py`（测试依赖放入 `test_requires`），经 `third-party/import.cmake` 导入为 CMake target，并链接到最窄消费模块；仅当依赖类型出现在 public header 时使用 `PUBLIC` / `INTERFACE`，禁止手工拼接 include 或库路径。
- **合规记录**：新增、替换或升级依赖时同步维护 `THIRD_PARTY_LICENSES.md`。

## 准入评估要点

引入新依赖前须先评估以下维度：

1. **许可证与分发合规**：与项目及现有第三方许可兼容，无传染性或分发限制。
2. **多平台静态体积**：所有生产依赖均静态链接并随跨平台制品分发，需控制各目标平台的二进制体积增量。
3. **平台与 ISA 兼容性**：对照[开发指南的平台与 ISA 基线](development_guide.md#平台与-isa-基线)，核验 Windows x64、Linux x64、Android arm64 与 macOS arm64 的可构建性及编译期 SIMD 宏假设。
4. **运行模型与 ABI 假设**：核验线程创建、静态/TLS 初始化顺序、异常、RTTI 与内存分配行为，避免引入无明确 owner 或 shutdown 协议的常驻线程，或破坏 C ABI 隐藏符号边界。
5. **持续维护成本**：评估上游活跃度、补丁基线维护难度及跨编译器（Clang / MSVC / NDK / Apple Clang）适配成本。

## 硬性接入约束

### 1. 源码静态构建与单一动态库交付

- 需要编译的生产依赖必须从源码经 CMake 构建并**静态链接**，最终只交付单一共享库（关闭上游 shared library、test、benchmark、example 等无关 target）。
- 纯头文件库跳过 CMake 构建，直接按头文件复制规则打包导出 CMake target。
- 非 CMake 的可编译依赖不切换上游 Meson/Autotools 等构建系统，须通过 Git patch 补写最小 `CMakeLists.txt` 纳入统一工具链构建。

### 2. 内存分配器与 `mimalloc` 边界

- 仅进入生产制品的依赖（含同时供生产和测试使用的依赖）需要分配器改造；纯测试或 benchmark 依赖免除此要求。
- 若依赖实际存在平台分配器调用（`malloc` / `calloc` / `realloc` / `free`、对齐分配等），必须通过源码 patch 替换为 `mi_*` 并注入 `mimalloc-static`；未实际替换分配器调用的依赖不凭空附加 `mimalloc` 依赖。
- 当前 `mimalloc` 配方关闭 `MI_OVERRIDE`、`MI_WIN_REDIRECT` 与 macOS interpose/zone 接管，不接管宿主 JVM 的进程级分配器；第三方依赖同样**不得重写全局 `operator new` / `operator delete`** 争夺最终产物的全局分配入口。
- 平台 API 隐式分配且契约要求标准 `free`（或专用释放函数）的内存，必须保留原释放路径，严禁混用 `mimalloc` 与平台 allocator。

### 3. Android 双身份兼容

- Android 构建同时保留原生工具链路径（`settings.os = Android` / CMake `ANDROID`）与 Linux 基础的显式第二身份路径（`user.target:os = android` / `TARGET_OS = ANDROID`）。
- 凡是对 Android 存在源码、兼容子库、编译定义或头文件特化的依赖，必须通过配方与 Git patch 同时识别这两条路径，并在验证中覆盖原生 Android、Android 第二身份与普通 Linux 三条路径。

### 4. 验收门禁

- 依赖接入或修改分配器/头文件路径后，须通过目标平台完整构建（含最终 `ysm-lib-jni` 链接）、运行覆盖分配/重分配/失败清理/释放路径的最小测试，并使用 `llvm-nm` 对生产静态库或目标对象文件执行分配器符号审查。

---

如要添加或修改依赖，请转至[添加依赖细则](dependency-adding.md)。
