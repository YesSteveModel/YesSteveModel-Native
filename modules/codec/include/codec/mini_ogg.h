#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include <absl/status/statusor.h>

#include "buffer.h"

namespace ysm::codec {
inline constexpr std::size_t kOggPageHeaderSize = 27;
inline constexpr std::size_t kOggMaxSegments = 255;
inline constexpr std::size_t kOggMaxPageSize =
    kOggPageHeaderSize + kOggMaxSegments + kOggMaxSegments * 255;
inline constexpr std::uint64_t kOggUnknownGranule =
    (std::numeric_limits<std::uint64_t>::max)();

struct OggPageView {
    BufferViewR bytes;
    BufferViewR lacing;
    BufferViewR body;
    std::uint64_t granule_position{};
    std::uint32_t serial_number{};
    std::uint32_t sequence{};
    bool continuation{};
    bool bos{};
    bool eos{};
};

[[nodiscard]] std::uint32_t OggCrc(BufferViewR page) noexcept;

// Parses and validates exactly the first complete page. Trailing bytes remain
// outside the returned page view and are consumed by the stream owner.
[[nodiscard]] absl::StatusOr<OggPageView> ReadOggPage(BufferViewR input);
}  // namespace ysm::codec
