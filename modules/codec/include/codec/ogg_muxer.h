#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "buffer.h"
#include "codec/mini_ogg.h"

namespace ysm::codec {
struct OggMuxPacket {
    BufferViewR bytes;
    std::uint64_t granule_position{};
};

struct OggMuxCursor {
    std::size_t packet_index{};
    std::size_t packet_offset{};

    friend bool operator==(const OggMuxCursor&, const OggMuxCursor&) = default;
};

struct OggMuxFragment {
    std::size_t packet_index{};
    std::size_t packet_offset{};
    std::uint16_t size{};
};

struct OggPagePlan {
    std::array<Byte, kOggMaxSegments> lacing{};
    std::array<OggMuxFragment, kOggMaxSegments> fragments{};
    std::size_t segment_count{};
    std::size_t body_size{};
    std::uint64_t granule_position{kOggUnknownGranule};
    std::uint32_t serial_number{};
    std::uint32_t sequence{};
    bool continuation{};
    bool bos{};
    bool eos{};
    OggMuxCursor next{};

    [[nodiscard]] std::size_t size() const noexcept {
        return kOggPageHeaderSize + segment_count + body_size;
    }
};

[[nodiscard]] absl::StatusOr<OggPagePlan> PlanOggPage(
    std::span<const OggMuxPacket> packets, OggMuxCursor cursor,
    std::size_t page_limit, std::uint32_t serial_number, std::uint32_t sequence,
    bool bos, bool end_stream);

[[nodiscard]] absl::Status WriteOggPage(const OggPagePlan& plan,
                                        std::span<const OggMuxPacket> packets,
                                        BufferView destination);
}  // namespace ysm::codec
