# 开发指南

[基建总览](infrastructure.md)：尽量复用现有基建。
- 现有公开门面能够承担职责时，必须复用，不在业务模块复制 helper 或建立平行抽象。
- 契约不足时，优先扩展原门面。只有职责、生命周期或性能约束不同且无法合理扩展时，才新增机制，并说明不能复用的原因与维护成本。

## 语言与设计

- 使用 C++23；禁止 coroutine，包括 `co_await`、`co_yield`、`co_return` 和自建 coroutine runtime。
- 遵循 Google C++ Style Guide；使用四空格缩进；行宽是软约束，换行优先保证可读性。
- 项目定义的所有 C++ 符号必须位于 `ysm` 或其子命名空间中。
- `std::string` 中的文本统一使用 UTF-8；仅为减少拷贝、提高性能而保留系统编码时，变量名必须显式包含 `native` 标识，以区别于 UTF-8 文本。
- 使用 `std::string_view` 或 `CStringView` 表示字符串借用，避免使用 `const std::string&`、`const char[]` 或 `const char*`。
- 字符串常量应使用 `constexpr CStringView` 定义。
- 若字符串字面量的接收类型不是 `const char*`，应使用自定义字面量后缀 `_sv`；该后缀定义于 `<c_string_view.h>`，位于 `ysm` 命名空间。
- 以函数式、声明式组合为主。仅在需要封装所有权、生命周期、持续状态、不变量或行为契约时引入 class。
- 尽量避免虚函数；编译期多态使用重载、模板和 CRTP 等模式，运行时多态使用 Microsoft Proxy。
- 禁止 `dynamic_cast`；允许 `std::any_cast`。
- public header 中的复杂成员类型和内部实现依赖尽量使用项目 [Static PImpl](infrastructure/pimpl.md) 隔离。
- 避免裸指针：
  - owning 使用直接对象或智能指针；borrow 使用引用。
  - 可空 owning 使用 `std::optional` 或智能指针；可空 borrow 使用 [`gch::optional_ref`](infrastructure/optional-ref.md)。
  - STL 容器需要存储不可空 borrow 时，使用 `std::reference_wrapper<T>` 作为元素类型。
  - 使用 `std::array` 代替 C 数组。
  - 使用 `std::span` 表示连续内存访问。
  - C 库边界必须补充所有权与生命周期契约，并在适配层立即把裸指针转换或封装为安全类型，不向业务代码传播。
- 需要智能指针时，优先使用 `std::unique_ptr`。
- 使用智能指针，若无法从上下文或对象类型语义推断可空性，必须由接口/参数/变量名称来显式反映。
- 避免 `std::unordered_map` / `std::unordered_set`，使用 `absl::flat_hash_map` / `absl::flat_hash_set`。
- 项目代码只使用 `enum class`；wire、JNI、bit-pack 和 ABI 边界显式指定底层类型。生成代码与第三方代码除外。
- 能由函数、模板或 `constexpr` 表达的能力不新增宏。
- 头文件中的私有宏使用完毕后必须 `#undef`，避免经 `#include` 传播到其他头文件或源文件。

## 错误模型与热点路径

- 可恢复失败使用 `absl::Status` / `absl::StatusOr<T>`。
- C++ 异常仅作为罕见 corner case 的兜底，例如分配失败或无法返回 status 的第三方接口，且只在它能降低 happy path 复杂度时使用；在最近的可靠边界收敛，不跨 JNI 传播。
- 渲染、动画、实时音频等热点内层路径禁止异常和 `Status`；二态结果用 `bool`，多态结果用小型 `enum class`，详细错误在冷路径处理。
- 不可信输入在进入 unchecked span、bit-unpack 取值、`YSM_ASSUME`、SIMD 假设或第三方 codec 前完成校验。

## 项目组织与依赖

- 代码按 CMake target 划分；跨模块声明位于 public include 边界，实现细节保持 private。
- 依赖由 Conan 2 配方解析并通过 CMake imported target 链接；不得手工拼接 include 或 library 路径。
- 所有 Conan 依赖必须构建并静态链接；生产构建只产出一个动态库，不携带其他动态库。
- 开发任务允许引入新依赖。引入前评估官方 target 的可构建性、许可证、体积、线程/初始化模型、异常/RTTI/ABI 假设及维护成本。
- 依赖链接到最窄的消费 target；仅当依赖类型出现在 public header 时使用 `PUBLIC` / `INTERFACE`。同步更新 Conan 配置、导入 target、锁定信息和 `THIRD_PARTY_LICENSES.md`。
- 现有通用库包括 Abseil、pystring（Python 语义字符串处理）、Microsoft Proxy（非继承式 type erasure）、optional_ref、magic_enum、cglm、yalantinglibs、mimalloc、cpu_features 和 Tracy。

## Java 符号依赖

- 除 JNI entry 声明、描述符和注册绑定所需的符号外，C++ 项目应避免显式依赖 Java 类、字段、构造器和方法符号，包括 JDK 类型。该原则覆盖符号名称、签名的硬编码、查找、缓存及调用；entry 函数体不因属于入口而豁免。
- JNI 自身提供的 string、array、direct buffer 操作不属于上述依赖。该例外不包括调用这些载体的 Java 方法，也不允许借对象数组引入 Java 业务对象的构造或字段访问。
- Java 对象构造、字段解释与方法组合应留在 Java。跨边界数据优先使用 JNI 原生载体；复杂结果由 native owner 持有，Java 构造包装或视图并闭合借用与释放责任。

该原则减少 native 对 Java 实现结构和符号查找的耦合；JNI 注册仍显式声明双方约定的入口。

## ABI、符号与生命周期

- 动态库默认隐藏符号，只导出外部 ABI 入口。所有导出均使用 C ABI；禁止导出 C++ ABI、普通工具、class、模板实例或第三方符号。
- 导出入口使用 `extern "C"`、固定宽度标量、显式 buffer 或 opaque handle；不得跨动态库/JNI 边界暴露 STL、异常、`absl::Status` 或 C++ owner。
- 新增导出时同步维护平台 export list / version script，并明确兼容责任。
- 除明确要求使用平台 C allocator 的路径外，禁止直接调用 `malloc`、`calloc`、`realloc` 和 `free`。进入生产制品的 Conan 依赖中的此类调用也必须通过配置或 patch 替换为项目分配器；仅供测试或性能基准使用的依赖不要求接入 mimalloc。
- `thread_local` 对象的构造或析构路径存在直接或间接动态分配时，其 allocator-aware 成员必须使用 `SystemAllocator`，包括嵌套 string/vector。
- runtime、日志、CPU feature 和 JavaVM 状态使用显式初始化，不依赖跨 translation unit 的静态或 `thread_local` 初始化顺序。
- 不新增无明确 owner、停止协议、背压和 shutdown 顺序的长期线程或队列。

## 验证

修改 public 声明、模板、宏、序列化布局、JNI descriptor、ISA 分派或 allocator 时，检查全部调用方和受支持平台。新增或变更契约应覆盖成功、边界和失败语义；性能路径的复杂化需要 benchmark 或 profiling 数据。
