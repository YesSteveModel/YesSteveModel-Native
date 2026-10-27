#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <absl/status/statusor.h>

#include "buffer.h"
#include "non_copyable.h"
#include "pimpl.h"

namespace ysm::codec {
class OpusAudioStream;

class OpusAudioCheckpoint final : public NonCopyable {
    YSM_PIMPL_DECLARE(Impl, 160)

   public:
    ~OpusAudioCheckpoint();

    [[nodiscard]] std::size_t OwnedBytes() const noexcept;
    [[nodiscard]] std::uint64_t EmittedFrames() const noexcept;

   private:
    friend class OpusAudioStream;
    OpusAudioCheckpoint();
};

enum class OpusFeedStatus : std::uint8_t {
    kProgress,
    kNeedInput,
    kOutputReady,
};

struct OpusFeedResult {
    std::size_t consumed{};
    OpusFeedStatus status{OpusFeedStatus::kNeedInput};
};

class OpusAudioStream : public NonCopyable {
    YSM_PIMPL_DECLARE(Impl, 512)

   public:
    static constexpr std::int32_t kError = -1;
    static constexpr std::int32_t kNeedInput = -2;

    explicit OpusAudioStream(std::uint64_t expected_frames);
    explicit OpusAudioStream(const OpusAudioCheckpoint& checkpoint);
    ~OpusAudioStream();

    // Capture requires complete headers, a whole-page boundary, drained PCM,
    // and live input. An unfinished packet at that boundary is preserved.
    [[nodiscard]] absl::StatusOr<std::size_t> CheckpointSize() const;
    [[nodiscard]] absl::StatusOr<std::unique_ptr<OpusAudioCheckpoint>>
    CaptureCheckpoint() const;

    // Borrows input only for this call. The caller advances by consumed and
    // retains the unconsumed suffix until a later feed.
    [[nodiscard]] absl::StatusOr<OpusFeedResult> Feed(BufferViewR input);

    void EndInput() noexcept;

    // Returns produced PCM bytes, kNeedInput, kError, or zero after a verified
    // real EOF. PCM is 48 kHz mono signed 16-bit little-endian.
    [[nodiscard]] std::int32_t Decode(BufferView destination);
};
}  // namespace ysm::codec
