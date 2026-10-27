#include "codec/mini_ogg.h"

#include <array>
#include <limits>

#include <absl/status/status.h>

namespace ysm::codec {
namespace {
constexpr std::array<Byte, 4> kCapturePattern{'O', 'g', 'g', 'S'};

std::uint32_t ReadLe32(BufferViewR bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           static_cast<std::uint32_t>(bytes[1]) << 8U |
           static_cast<std::uint32_t>(bytes[2]) << 16U |
           static_cast<std::uint32_t>(bytes[3]) << 24U;
}

std::uint64_t ReadLe64(BufferViewR bytes) noexcept {
    return static_cast<std::uint64_t>(ReadLe32(bytes)) |
           static_cast<std::uint64_t>(ReadLe32(bytes.subspan(4))) << 32U;
}
}  // namespace

std::uint32_t OggCrc(BufferViewR page) noexcept {
    std::uint32_t crc = 0;
    for (std::size_t index = 0; index < page.size(); ++index) {
        const auto value = index >= 22 && index < 26 ? Byte{0} : page[index];
        crc ^= static_cast<std::uint32_t>(value) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = crc << 1U ^ ((crc & 0x80000000U) == 0 ? 0U : 0x04C11DB7U);
        }
    }
    return crc;
}

absl::StatusOr<OggPageView> ReadOggPage(BufferViewR input) {
    if (input.size() < kOggPageHeaderSize) {
        return absl::OutOfRangeError("Incomplete Ogg page header");
    }
    if (!Cmp(input.first<kCapturePattern.size()>(), kCapturePattern)) {
        return absl::DataLossError("Invalid Ogg capture pattern");
    }
    if (input[4] != 0 || (input[5] & 0xf8U) != 0) {
        return absl::DataLossError("Unsupported Ogg page header");
    }

    const auto segment_count = static_cast<std::size_t>(input[26]);
    const auto header_size = kOggPageHeaderSize + segment_count;
    if (input.size() < header_size) {
        return absl::OutOfRangeError("Incomplete Ogg lacing table");
    }
    const auto lacing = input.subspan(kOggPageHeaderSize, segment_count);
    std::size_t body_size = 0;
    for (const auto value : lacing) {
        body_size += value;
    }
    if (body_size > kOggMaxPageSize - header_size ||
        input.size() - header_size < body_size) {
        return absl::OutOfRangeError("Incomplete Ogg page body");
    }

    const auto page_size = header_size + body_size;
    const auto page = input.first(page_size);
    if (OggCrc(page) != ReadLe32(page.subspan(22, 4))) {
        return absl::DataLossError("Ogg page CRC mismatch");
    }

    const auto flags = page[5];
    return OggPageView{
        .bytes = page,
        .lacing = lacing,
        .body = page.subspan(header_size, body_size),
        .granule_position = ReadLe64(page.subspan(6, 8)),
        .serial_number = ReadLe32(page.subspan(14, 4)),
        .sequence = ReadLe32(page.subspan(18, 4)),
        .continuation = (flags & 0x01U) != 0,
        .bos = (flags & 0x02U) != 0,
        .eos = (flags & 0x04U) != 0,
    };
}
}  // namespace ysm::codec
