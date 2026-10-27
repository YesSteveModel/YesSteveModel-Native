# Codec 基建

[返回基建总览](../infrastructure_cn.md)

主要头文件：`<codec/blake3.h>`、`<codec/base64.h>`、`<codec/hex.h>`、`<codec/zstd.h>`、`<codec/zlib.h>`、`<codec/image.h>`、`<codec/archive.h>`、`<codec/audio.h>`、`<codec/java_random.h>`。

codec API 读取 `BufferViewR`，写入调用方 buffer 或 `BufferManaged`。输出 buffer 必须按实际写入长度截断。

## 哈希与文本编码

- `Blake3Hasher` 支持分段 `Update`、32-byte `Finalize` 和 `Reset`；一次性输入使用 `Blake3Hash`。
- `Base64Encode/Decode` 使用 `OutputMaxSize` 分配；空输入写入 0 byte。
- `HexEncode/Decode` 使用精确 `OutputSize`；decode 拒绝奇数长度和非法字符。

## 压缩

- `ZstdGetCompressMaxSize` 返回压缩上界；`ZstdCompress` 返回实际长度并校验 level。
- `ZstdDecompress(input, BufferManaged&)` 自动增长但不超过 256 MiB。
- fixed-buffer zstd/zlib 变体要求调用方提供足够容量。

解压成功不表示内容完整；不可信数据还需校验预期尺寸或 hash。

## 图片

使用 `ImageProbe` 获得尺寸，校验 `width * height * 4` 后分配 RGBA buffer，再调用 `ImageDecode`。`ImageEncodeLossy` / `ImageEncodeLossless` 返回 `EncodeResult{info, size}`；有效输出是 `size` 指定的前缀。

具体 decoder/encoder 供格式门面实现和测试使用；普通调用方不直接选择平台 codec。

## 归档

`codec::Archive<Zip>` / `codec::Archive<SevenZip>` 提供 `Ok`、`Files`、`Directories`、路径查询和 `Extract`。构造后先检查 `Ok()`；entry 和目录 view 不得超出 archive 生命周期。

用 `BufferViewR` 构造时借用输入 backing，调用方必须保活且不修改这段 bytes，直到 archive 销毁；路径构造由 archive 管理文件访问。`Extract` 写入调用方的 `BufferManaged`，输出由调用方拥有。

目录树会反复折叠“没有直接文件且只有一个子目录”的根节点。枚举和字符串查询相对折叠后的根；entry 的 `FullName()` 仍是包内完整名称。例如包中仅有：

```text
bundle/model/geometry.json
bundle/model/textures/main.png
```

此时 `Files()` 枚举 `geometry.json`，`Directories()` 枚举 `textures`；查询使用 `geometry.json` 或 `textures/main.png`，对应 entry 的 `FullName()` 仍带 `bundle/model/`。不要把 `FullName()` 原样当作折叠后的查询路径。

entry 名称是不可信路径。写入文件系统前拒绝绝对路径、`..`、平台分隔符绕过和目标目录逃逸。

## 音频与兼容工具

Ogg/Opus 的输入、输出和结束协议见[Opus 流解码](opus_cn.md)，生成期 inspect、R/T 候选与验证见[音频规范化](audio-normalization_cn.md)。`JavaRandom` 仅用于复现 Java `Random` 序列，不是密码学 RNG。
