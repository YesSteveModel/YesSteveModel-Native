#include "codec/ogg_muxer.h"

#include <algorithm>
#include <limits>

namespace ysm::codec {
namespace {
void WriteLe32(BufferView destination, std::uint32_t value) noexcept {
    for (int index = 0; index < 4; ++index) {
        destination[static_cast<std::size_t>(index)] =
            static_cast<Byte>(value >> (index * 8));
    }
}

void WriteLe64(BufferView destination, std::uint64_t value) noexcept {
    WriteLe32(destination, static_cast<std::uint32_t>(value));
    WriteLe32(destination.subspan(4), static_cast<std::uint32_t>(value >> 32U));
}
}  // namespace

absl::StatusOr<OggPagePlan> PlanOggPage(std::span<const OggMuxPacket> packets,
                                        OggMuxCursor cursor,
                                        std::size_t page_limit,
                                        std::uint32_t serial_number,
                                        std::uint32_t sequence, bool bos,
                                        bool end_stream) {
    if (packets.empty() || cursor.packet_index >= packets.size() ||
        page_limit > kOggMaxPageSize || page_limit < kOggPageHeaderSize + 1) {
        return absl::InvalidArgumentError("Invalid Ogg page plan input");
    }
    if (cursor.packet_offset > packets[cursor.packet_index].bytes.size()) {
        return absl::InvalidArgumentError("Invalid Ogg packet cursor");
    }

    OggPagePlan plan;
    plan.serial_number = serial_number;
    plan.sequence = sequence;
    plan.continuation = cursor.packet_offset != 0;
    plan.bos = bos && cursor.packet_index == 0 && cursor.packet_offset == 0;
    plan.next = cursor;

    while (plan.next.packet_index < packets.size() &&
           plan.segment_count < kOggMaxSegments) {
        const auto& packet = packets[plan.next.packet_index].bytes;
        if (plan.next.packet_offset > packet.size()) {
            return absl::InvalidArgumentError("Invalid Ogg packet cursor");
        }
        const auto remaining = packet.size() - plan.next.packet_offset;
        const auto fragment_size = std::min<std::size_t>(remaining, 255);
        const auto candidate_size = kOggPageHeaderSize + plan.segment_count +
                                    1 + plan.body_size + fragment_size;
        if (candidate_size > page_limit) {
            break;
        }

        const auto segment = plan.segment_count++;
        plan.lacing[segment] = static_cast<Byte>(fragment_size);
        plan.fragments[segment] = {
            .packet_index = plan.next.packet_index,
            .packet_offset = plan.next.packet_offset,
            .size = static_cast<std::uint16_t>(fragment_size),
        };
        plan.body_size += fragment_size;
        plan.next.packet_offset += fragment_size;

        if (fragment_size < 255) {
            plan.granule_position =
                packets[plan.next.packet_index].granule_position;
            ++plan.next.packet_index;
            plan.next.packet_offset = 0;
        }
    }

    if (plan.segment_count == 0) {
        return absl::ResourceExhaustedError(
            "Ogg page limit cannot hold the next lacing segment");
    }
    plan.eos = end_stream && plan.next.packet_index == packets.size();
    return plan;
}

absl::Status WriteOggPage(const OggPagePlan& plan,
                          std::span<const OggMuxPacket> packets,
                          BufferView destination) {
    if (destination.size() != plan.size() || plan.segment_count == 0 ||
        plan.segment_count > kOggMaxSegments ||
        plan.body_size > destination.size()) {
        return absl::InvalidArgumentError("Invalid Ogg page destination");
    }

    destination[0] = 'O';
    destination[1] = 'g';
    destination[2] = 'g';
    destination[3] = 'S';
    destination[4] = 0;
    destination[5] =
        static_cast<Byte>((plan.continuation ? 0x01U : 0U) |
                          (plan.bos ? 0x02U : 0U) | (plan.eos ? 0x04U : 0U));
    WriteLe64(destination.subspan(6, 8), plan.granule_position);
    WriteLe32(destination.subspan(14, 4), plan.serial_number);
    WriteLe32(destination.subspan(18, 4), plan.sequence);
    WriteLe32(destination.subspan(22, 4), 0);
    destination[26] = static_cast<Byte>(plan.segment_count);
    std::copy_n(plan.lacing.begin(), plan.segment_count,
                destination.begin() + kOggPageHeaderSize);

    auto body = destination.subspan(kOggPageHeaderSize + plan.segment_count);
    std::size_t written = 0;
    for (std::size_t index = 0; index < plan.segment_count; ++index) {
        const auto& fragment = plan.fragments[index];
        if (fragment.packet_index >= packets.size()) {
            return absl::InvalidArgumentError("Invalid Ogg page fragment");
        }
        const auto packet = packets[fragment.packet_index].bytes;
        if (fragment.packet_offset > packet.size() ||
            fragment.size > packet.size() - fragment.packet_offset ||
            fragment.size > body.size() - written) {
            return absl::InvalidArgumentError("Invalid Ogg page fragment");
        }
        std::copy_n(packet.begin() + fragment.packet_offset, fragment.size,
                    body.begin() + written);
        written += fragment.size;
    }
    if (written != plan.body_size) {
        return absl::InternalError("Ogg page plan body mismatch");
    }
    WriteLe32(destination.subspan(22, 4), OggCrc(destination));
    return absl::OkStatus();
}
}  // namespace ysm::codec
