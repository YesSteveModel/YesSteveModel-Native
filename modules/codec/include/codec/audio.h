#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/functional/function_ref.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "buffer.h"
#include "buffer_managed.h"

namespace ysm::codec {
enum class AudioCodec : std::uint8_t {
    kOpus,
    kVorbis,
};

struct AudioFacts {
    AudioCodec codec{AudioCodec::kOpus};
    std::uint8_t channels{};
    std::uint32_t sample_rate{};
    std::uint64_t frames{};
    std::uint64_t audio_packet_bytes{};
    std::uint32_t serial_number{};
    std::uint16_t pre_skip{};
    std::int16_t output_gain{};
};

[[nodiscard]] absl::StatusOr<AudioFacts> InspectAudio(BufferViewR source);

enum class NormalizationRole : std::uint8_t {
    kRetained,
    kTranscoded,
};

enum class AudioFailureKind : std::uint8_t {
    kSourceInvalid,
    kUnsupportedProfile,
    kOptionalCandidateFailure,
    kEnvironmentFailure,
    kCancelled,
};

enum class AudioStage : std::uint8_t {
    kInspect,
    kDecode,
    kResample,
    kEncode,
    kMux,
    kValidate,
};

struct AudioFailure {
    AudioFailureKind kind{AudioFailureKind::kEnvironmentFailure};
    AudioStage stage{AudioStage::kInspect};
    absl::Status status;
};

struct NormalizedAudio {
    AudioFacts facts;
    NormalizationRole role{NormalizationRole::kRetained};
    BufferManaged first_page;
    std::vector<BufferManaged> chunks;
};

class NormalizeAudioResult {
   public:
    static NormalizeAudioResult Success(NormalizedAudio value);
    static NormalizeAudioResult Failure(AudioFailure failure);

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }

    [[nodiscard]] const NormalizedAudio& value() const& { return *value_; }

    [[nodiscard]] NormalizedAudio&& value() && { return std::move(*value_); }

    [[nodiscard]] const AudioFailure& failure() const& { return failure_; }

   private:
    std::optional<NormalizedAudio> value_;
    AudioFailure failure_;
};

struct NormalizeAudioOptions {
    NormalizationRole role{NormalizationRole::kRetained};
    std::uint32_t transcoded_serial{};
    std::size_t page_limit{57'344};
};

[[nodiscard]] NormalizeAudioResult NormalizeAudio(
    BufferViewR source, const NormalizeAudioOptions& options,
    absl::FunctionRef<bool()> cancelled);

[[nodiscard]] absl::Status ValidatePrefix(const NormalizedAudio& audio);
[[nodiscard]] absl::Status ValidateComplete(const NormalizedAudio& audio);
}  // namespace ysm::codec
