#pragma once

#include <absl/status/statusor.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <buffer.h>
#include <buffer_managed.h>

namespace ysm::legacy::v3 {
enum class PayloadKind : std::uint16_t {
    kManifest = 1,
    kStringData = 2,
    kModelData = 3,
    kBlobImage = 4,
    kNamedImage = 5,
    kSoundStream = 6,
};

enum class PayloadEncoding : std::uint16_t {
    kDirect = 0,
    kRgba = 1,
    kPng = 2,
    kJpeg = 3,
    kWebp = 4,
    kAvif = 5,
    kOggVorbis = 6,
    kOggOpus = 7,
    kZtx = 8,
};

struct ImportMetadata {
    std::uint32_t inner_version{};
    std::uint64_t source_size{};
    std::array<Byte, 32> model_id{};
};

struct ImageMetadata {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t frame_count{};
};

struct SoundMetadata {
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
    std::uint64_t frames{};
};

struct Payload {
    PayloadKind kind{};
    PayloadEncoding encoding{};
    std::uint32_t logical_id{};
    std::string name;
    std::optional<ImageMetadata> image;
    BufferManaged bytes;
    std::optional<SoundMetadata> sound;
};

struct ImportResult {
    ImportMetadata metadata;
    std::vector<Payload> payloads;
};

// The caller owns source lifetime for the duration of this synchronous read.
absl::StatusOr<ImportResult> Import(BufferViewR source);
}  // namespace ysm::legacy::v3
