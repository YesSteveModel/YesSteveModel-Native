# 音频规范化

[返回基建总览](../infrastructure_cn.md)

主要头文件：`<codec/audio.h>`。`InspectAudio`、`NormalizeAudio`、`ValidatePrefix` 和 `ValidateComplete` 是生成期音频入口；Java 的同步入口是 `AudioNormalizer`。容器选择、计价、存储和发布仍由 Java 拥有。

## 输入与候选

输入必须是完整、单 logical stream 的 Ogg/Opus 或 Ogg/Vorbis。Inspector 校验 page CRC、serial、sequence、continuation、EOS、packet profile 和解码时间轴；识别到 codec 签名但 profile 不受支持与损坏来源分别返回 `UnsupportedProfile` 和 `SourceInvalid`。

`NormalizationRole::kRetained` 只接受已支持的 Opus：保留音频 packet bytes、output gain 和精确裁剪时间轴，清除非规范 metadata，并重新分页。`kTranscoded` 解码 Opus/Vorbis，双声道等权下混，非 48 kHz 经 SpeexDSP quality 5 重采样，再编码为 48 kHz mono Opus。Opus 来源的 output gain 只写入新 OpusHead，不在转码 PCM 中预应用；R128 tag 不进入 T。

`NormalizedAudio::first_page` 仅含独立 OpusHead page；`chunks.front()` 包含 tags 及产生首个有效 PCM 所需的完整 page，普通 chunk 上限由 `page_limit` 指定且不得超过 57,344 bytes。每个候选发布前必须通过 `ValidateComplete`；`ValidatePrefix` 只证明首页和首块可以在不宣告 EOF 的情况下产出有效 PCM。

## 时间轴与失败

公共 `AudioFacts::frames` 是扣除 Opus pre-skip/尾裁剪或 Vorbis EOS 后的有效 frame。转码长度使用整数半向上舍入：`round(source_frames * 48000 / source_rate)`。Ogg granule 是权威时间轴；调用方提供的 frame 数只参与一致性检查，不能覆盖来源 EOS。

`NormalizeAudioResult` 的失败同时保留 `AudioFailureKind`、`AudioStage` 和 `absl::Status`。取消在有限 packet/输出边界观察，失败不返回部分 candidate。JNI `AudioNormalizer.Result` 用单一 owner 保活首页和全部 direct chunk；结果关闭后不得继续使用其 `ByteBuffer` view，同一结果的读取与关闭必须串行。
