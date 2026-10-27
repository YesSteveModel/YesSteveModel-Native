# C++ 基建使用指南

使用基建时，优先依据本页链接的契约与示例，尽量不阅读实现。此为软约束：契约不清、文档与行为冲突、排障或修改基建时，可按具体问题回查源码；发现通用用法缺口时补回对应专题。

## 基建主题索引

| 主题 | 内容 | 所属 module | 首选入口 |
| --- | --- | --- | --- |
| [错误模型与控制流](infrastructure/error-handling_cn.md) | 状态传播与前置条件 | `core` | `YSM_RETURN_IF_ERROR`、`YSM_DECLARE_OR_RETURN`、`YSM_ASSERT` |
| [Buffer、所有权与字节视图](infrastructure/buffer-ownership_cn.md) | buffer、字符串、路径与文件 I/O | `core` | `BufferManaged`、`BufferViewR`、`FileRead` |
| [`gch::optional_ref` 可空借用](infrastructure/optional-ref_cn.md) | 可空引用、生命周期与访问约束 | 通用依赖 | `gch::optional_ref<T>`、`gch::optional_cref<T>` |
| [PImpl 与实现依赖隔离](infrastructure/pimpl_cn.md) | inline storage、容量与对齐、特殊成员函数 | `core` | `YSM_PIMPL_DECLARE`、`YSM_PIMPL_CONSTRUCT`、`YSM_PIMPL_DEFINITION` |
| [通用模板与 RAII](infrastructure/templates-raii_cn.md) | scope guard、枚举分派、位打包与层级树 | `core` | `ScopeGuard`、`EnumCast`、`YSM_BIT_PACK` |
| [宏与编译期设施](infrastructure/macros-compile-time_cn.md) | 日志、profiling、优化、ISA 与可见性 | `core` | `YSM_LOG`、`YSM_PROFILE_ZONE`、`YSM_ASSUME` |
| [CPU、SIMD](infrastructure/cpu-simd_cn.md) | CPU 探测、SIMD tag | `core` | `InitCpuInfo`、`simd::Tag` |
| [JNI 基建](infrastructure/jni_cn.md) | 引用、数组、buffer、handle 与 entry 注册 | `java` | `Ref<T>`、`BufferInput`、`YSM_JNI_ENTRY` |
| [Codec 基建](infrastructure/codec_cn.md) | 哈希、编码、压缩、图片与归档 | `codec` | `Blake3Hash`、`ZstdDecompress`、`ImageProbe` |
| [Opus 流解码](infrastructure/opus_cn.md) | 分片输入、PCM 输出与结束协议 | `codec` | `OpusAudioStream` |
| [音频规范化](infrastructure/audio-normalization_cn.md) | 严格 inspect、R/T 候选、时间轴与失败分类 | `codec` / `java` | `NormalizeAudio`、`AudioNormalizer` |
| [GFX 高级能力](infrastructure/gfx_cn.md) | 烘焙、baked cache、姿态、调度与渲染 | `gfx` | `BakeModel`、`ModelState::Extract`、`Render` |
