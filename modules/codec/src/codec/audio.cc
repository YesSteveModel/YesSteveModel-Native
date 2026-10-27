#include "codec/audio.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <speex/speex_resampler.h>
#include <vorbis/codec.h>

#include "codec/mini_ogg.h"
#include "codec/ogg_muxer.h"
#include "codec/opus.h"
#include "scope_guard.h"

namespace ysm::codec {
namespace {
constexpr std::size_t kMaxSourceSize = 128U * 1024U * 1024U;
constexpr std::array<Byte, 8> kOpusHead{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
constexpr std::array<Byte, 8> kOpusTags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
constexpr std::array<Byte, 7> kVorbisId{1, 'v', 'o', 'r', 'b', 'i', 's'};
constexpr std::uint32_t kNormalizedSampleRate = 48'000;
constexpr std::size_t kOpusFrameSize = 960;
constexpr opus_int32 kOpusBitrate = 70'000;

std::uint16_t ReadLe16(BufferViewR bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1]) << 8U;
}

std::uint32_t ReadLe32(BufferViewR bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           static_cast<std::uint32_t>(bytes[1]) << 8U |
           static_cast<std::uint32_t>(bytes[2]) << 16U |
           static_cast<std::uint32_t>(bytes[3]) << 24U;
}

void AppendLe32(std::vector<Byte>& output, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        output.push_back(static_cast<Byte>(value >> (index * 8)));
    }
}

void AppendLe16(std::vector<Byte>& output, std::uint16_t value) {
    output.push_back(static_cast<Byte>(value));
    output.push_back(static_cast<Byte>(value >> 8U));
}

void Append(BufferManaged& destination, BufferViewR source, std::size_t limit) {
    if (destination.size() > limit ||
        source.size() > limit - destination.size()) {
        throw std::bad_alloc();
    }
    const auto offset = destination.size();
    destination.resize(offset + source.size());
    std::memcpy(destination.data() + offset, source.data(), source.size());
}

struct StoredPacket {
    BufferViewR bytes;
    std::unique_ptr<BufferManaged> owned;
    std::uint64_t granule{kOggUnknownGranule};
};

struct ParsedPage {
    std::size_t first_packet{};
    std::size_t packet_count{};
    std::uint64_t granule{kOggUnknownGranule};
    bool eos{};
};

struct ParsedAudio {
    AudioFacts facts;
    std::vector<StoredPacket> packets;
    std::vector<ParsedPage> pages;
    std::optional<std::int16_t> r128_track;
    std::optional<std::int16_t> r128_album;
    std::uint64_t origin{};
    std::uint64_t final_granule{};
};

absl::Status AddOwnedPacket(std::vector<StoredPacket>& packets,
                            BufferManaged bytes) {
    auto owner = std::make_unique<BufferManaged>(std::move(bytes));
    const auto view = BufferViewR(*owner);
    packets.push_back(StoredPacket{.bytes = view, .owned = std::move(owner)});
    return absl::OkStatus();
}

absl::StatusOr<ParsedAudio> ParseContainer(BufferViewR source) {
    if (source.empty() || source.size() > kMaxSourceSize) {
        return absl::InvalidArgumentError("Audio source size is not admitted");
    }

    ParsedAudio parsed;
    BufferManaged partial_packet;
    std::size_t offset = 0;
    std::uint32_t serial = 0;
    std::uint32_t sequence = 0;
    bool has_stream = false;
    bool saw_eos = false;

    while (offset < source.size()) {
        auto page = ReadOggPage(source.subspan(offset));
        if (!page.ok()) {
            return page.status();
        }
        if (!has_stream) {
            if (!page->bos || page->continuation || page->sequence != 0) {
                return absl::DataLossError("Invalid first Ogg page");
            }
            has_stream = true;
            serial = page->serial_number;
        } else if (page->bos || page->serial_number != serial ||
                   page->sequence != sequence) {
            return absl::DataLossError(
                "Chained, multiplexed, or discontinuous Ogg stream");
        }
        ++sequence;
        if (saw_eos || page->continuation != !partial_packet.empty()) {
            return absl::DataLossError("Invalid Ogg stream continuation");
        }

        const auto first_packet = parsed.packets.size();
        std::size_t body_offset = 0;
        std::size_t packet_start = 0;
        bool assembling = !partial_packet.empty();
        for (const auto lacing_value : page->lacing) {
            const auto size = static_cast<std::size_t>(lacing_value);
            if (size > page->body.size() - body_offset) {
                return absl::DataLossError("Invalid Ogg lacing");
            }
            if (assembling) {
                Append(partial_packet, page->body.subspan(body_offset, size),
                       source.size());
            }
            body_offset += size;
            if (size == 255) {
                continue;
            }
            if (assembling) {
                auto status =
                    AddOwnedPacket(parsed.packets, std::move(partial_packet));
                if (!status.ok()) {
                    return status;
                }
                partial_packet = BufferManaged();
                assembling = false;
            } else {
                parsed.packets.push_back(StoredPacket{
                    .bytes = page->body.subspan(packet_start,
                                                body_offset - packet_start)});
            }
            packet_start = body_offset;
        }
        if (body_offset != page->body.size()) {
            return absl::DataLossError("Ogg body has unreferenced bytes");
        }
        if (!page->lacing.empty() && page->lacing.back() == 255 &&
            !assembling) {
            Append(partial_packet,
                   page->body.subspan(packet_start, body_offset - packet_start),
                   source.size());
        }

        const auto packet_count = parsed.packets.size() - first_packet;
        if (packet_count == 0) {
            if (page->granule_position != kOggUnknownGranule) {
                return absl::DataLossError(
                    "Ogg page without a completed packet has a granule");
            }
        } else {
            parsed.packets.back().granule = page->granule_position;
        }
        parsed.pages.push_back(ParsedPage{first_packet, packet_count,
                                          page->granule_position, page->eos});
        offset += page->bytes.size();
        if (page->eos) {
            if (!partial_packet.empty() || offset != source.size()) {
                return absl::DataLossError(
                    "Ogg EOS is incomplete or has trailing bytes");
            }
            saw_eos = true;
        }
    }
    if (!saw_eos || parsed.packets.empty()) {
        return absl::DataLossError("Ogg stream has no complete EOS");
    }
    parsed.facts.serial_number = serial;
    return parsed;
}

bool AsciiEqualFold(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto a = static_cast<unsigned char>(left[index]);
        auto b = static_cast<unsigned char>(right[index]);
        if (a >= 'a' && a <= 'z') {
            a -= 'a' - 'A';
        }
        if (b >= 'a' && b <= 'z') {
            b -= 'a' - 'A';
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

absl::StatusOr<std::int16_t> ParseR128(std::string_view value) {
    if (value.empty() || value.size() > 6) {
        return absl::DataLossError("Invalid R128 gain length");
    }
    const bool has_sign = value.front() == '+' || value.front() == '-';
    const auto digits = value.substr(has_sign ? 1 : 0);
    if (digits.empty()) {
        return absl::DataLossError("Invalid R128 gain value");
    }
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
            return absl::DataLossError("Invalid R128 gain value");
        }
    }
    if (value.front() == '+') {
        value.remove_prefix(1);
    }
    std::int32_t parsed = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        parsed < (std::numeric_limits<std::int16_t>::min)() ||
        parsed > (std::numeric_limits<std::int16_t>::max)()) {
        return absl::DataLossError("Invalid R128 gain value");
    }
    return static_cast<std::int16_t>(parsed);
}

absl::Status ParseOpusTags(BufferViewR packet, ParsedAudio& parsed) {
    if (packet.size() < 16 ||
        !Cmp(packet.first<kOpusTags.size()>(), kOpusTags)) {
        return absl::DataLossError("Invalid OpusTags packet");
    }
    std::size_t offset = kOpusTags.size();
    const auto vendor_size =
        static_cast<std::size_t>(ReadLe32(packet.subspan(offset, 4)));
    offset += 4;
    if (vendor_size > packet.size() - offset) {
        return absl::DataLossError("Truncated OpusTags vendor");
    }
    offset += vendor_size;
    if (packet.size() - offset < 4) {
        return absl::DataLossError("Truncated OpusTags comment count");
    }
    const auto comment_count = ReadLe32(packet.subspan(offset, 4));
    offset += 4;
    for (std::uint32_t index = 0; index < comment_count; ++index) {
        if (packet.size() - offset < 4) {
            return absl::DataLossError("Truncated OpusTags comment length");
        }
        const auto size =
            static_cast<std::size_t>(ReadLe32(packet.subspan(offset, 4)));
        offset += 4;
        if (size > packet.size() - offset) {
            return absl::DataLossError("Truncated OpusTags comment");
        }
        const auto comment = std::string_view(
            reinterpret_cast<const char*>(packet.data() + offset), size);
        offset += size;
        const auto separator = comment.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }
        const auto key = comment.substr(0, separator);
        const auto value = comment.substr(separator + 1);
        std::optional<std::int16_t>* target = nullptr;
        if (AsciiEqualFold(key, "R128_TRACK_GAIN")) {
            target = &parsed.r128_track;
        } else if (AsciiEqualFold(key, "R128_ALBUM_GAIN")) {
            target = &parsed.r128_album;
        } else {
            continue;
        }
        if (target->has_value()) {
            return absl::DataLossError("Duplicate R128 gain tag");
        }
        auto gain = ParseR128(value);
        if (!gain.ok()) {
            return gain.status();
        }
        *target = *gain;
    }
    if (offset < packet.size() && (packet[offset] & 1U) != 0) {
        return absl::UnimplementedError(
            "OpusTags contains a required unknown extension");
    }
    return absl::OkStatus();
}

absl::Status ValidateOpusPackets(const ParsedAudio& parsed) {
    int error = OPUS_OK;
    auto* decoder = opus_decoder_create(48'000, parsed.facts.channels, &error);
    if (decoder == nullptr || error != OPUS_OK) {
        if (decoder != nullptr) {
            opus_decoder_destroy(decoder);
        }
        return absl::ResourceExhaustedError(
            "Failed to allocate Opus validation decoder");
    }
    ScopeGuard close([decoder] { opus_decoder_destroy(decoder); });
    std::array<opus_int16, 5'760 * 2> pcm{};
    for (std::size_t index = 2; index < parsed.packets.size(); ++index) {
        const auto packet = parsed.packets[index].bytes;
        if (packet.size() > static_cast<std::size_t>(
                                (std::numeric_limits<opus_int32>::max)())) {
            return absl::DataLossError("Opus packet is too large");
        }
        const auto frames = opus_packet_get_nb_samples(
            packet.data(), static_cast<opus_int32>(packet.size()), 48'000);
        if (frames <= 0 || opus_decode(decoder, packet.data(),
                                       static_cast<opus_int32>(packet.size()),
                                       pcm.data(), frames, 0) != frames) {
            return absl::DataLossError("Invalid Opus packet payload");
        }
    }
    return absl::OkStatus();
}

absl::Status InspectOpus(ParsedAudio& parsed) {
    if (parsed.packets.size() < 3) {
        return absl::DataLossError("Opus stream lacks required packets");
    }
    const auto head = parsed.packets[0].bytes;
    if (head.size() != 19 || !Cmp(head.first<kOpusHead.size()>(), kOpusHead) ||
        head[8] > 15 || (head[9] != 1 && head[9] != 2) || head[18] != 0) {
        return absl::UnimplementedError("Unsupported Opus profile");
    }
    parsed.facts.codec = AudioCodec::kOpus;
    parsed.facts.channels = head[9];
    parsed.facts.sample_rate = 48'000;
    parsed.facts.pre_skip = ReadLe16(head.subspan(10, 2));
    parsed.facts.output_gain =
        static_cast<std::int16_t>(ReadLe16(head.subspan(16, 2)));
    auto tags = ParseOpusTags(parsed.packets[1].bytes, parsed);
    if (!tags.ok()) {
        return tags;
    }

    std::uint64_t decoded_frames = 0;
    std::uint64_t origin = 0;
    std::uint64_t previous_granule = 0;
    bool has_origin = false;
    bool has_granule = false;
    bool saw_audio_eos = false;

    for (const auto& page : parsed.pages) {
        std::uint64_t page_frames = 0;
        bool has_audio = false;
        const auto end = page.first_packet + page.packet_count;
        for (auto index = page.first_packet; index < end; ++index) {
            if (index < 2) {
                continue;
            }
            has_audio = true;
            const auto packet = parsed.packets[index].bytes;
            if (packet.size() > static_cast<std::size_t>(
                                    (std::numeric_limits<opus_int32>::max)())) {
                return absl::DataLossError("Opus packet is too large");
            }
            const auto frames = opus_packet_get_nb_samples(
                packet.data(), static_cast<opus_int32>(packet.size()), 48'000);
            if (frames <= 0 ||
                page_frames > (std::numeric_limits<std::uint64_t>::max)() -
                                  static_cast<std::uint64_t>(frames)) {
                return absl::DataLossError("Invalid Opus packet duration");
            }
            page_frames += static_cast<std::uint64_t>(frames);
            if (parsed.facts.audio_packet_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - packet.size()) {
                return absl::DataLossError("Opus packet byte count overflow");
            }
            parsed.facts.audio_packet_bytes += packet.size();
        }
        if (!has_audio) {
            continue;
        }
        if (page.granule == kOggUnknownGranule ||
            decoded_frames >
                (std::numeric_limits<std::uint64_t>::max)() - page_frames) {
            return absl::DataLossError("Invalid Opus page timeline");
        }
        const auto decoded_after = decoded_frames + page_frames;
        if (!has_origin) {
            if (page.eos && page.granule < decoded_after) {
                origin = 0;
            } else {
                if (page.granule < decoded_after) {
                    return absl::DataLossError("Invalid initial Opus granule");
                }
                origin = page.granule - decoded_after;
            }
            has_origin = true;
        }
        if (page.granule < origin ||
            (has_granule && page.granule < previous_granule)) {
            return absl::DataLossError("Regressing Opus granule");
        }
        const auto page_end = page.granule - origin;
        if ((!page.eos && page_end != decoded_after) ||
            (page.eos &&
             (page_end > decoded_after || page_end < decoded_frames))) {
            return absl::DataLossError("Inconsistent Opus granule");
        }
        previous_granule = page.granule;
        has_granule = true;
        decoded_frames = decoded_after;
        if (page.eos) {
            saw_audio_eos = true;
            parsed.final_granule = page.granule;
        }
    }
    if (!saw_audio_eos || parsed.final_granule < origin ||
        parsed.final_granule - origin <= parsed.facts.pre_skip) {
        return absl::DataLossError("Opus stream has no effective PCM");
    }
    parsed.origin = origin;
    parsed.facts.frames = parsed.final_granule - origin - parsed.facts.pre_skip;
    return ValidateOpusPackets(parsed);
}

struct VorbisState {
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    bool dsp_initialized{};
    bool block_initialized{};

    VorbisState() {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }

    ~VorbisState() {
        if (block_initialized) {
            vorbis_block_clear(&block);
        }
        if (dsp_initialized) {
            vorbis_dsp_clear(&dsp);
        }
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }
};

absl::StatusOr<ogg_packet> VorbisPacket(const StoredPacket& packet,
                                        std::size_t index, bool eos) {
    if (packet.bytes.size() >
        static_cast<std::size_t>((std::numeric_limits<long>::max)())) {
        return absl::ResourceExhaustedError("Vorbis packet is too large");
    }
    return ogg_packet{
        .packet = const_cast<unsigned char*>(packet.bytes.data()),
        .bytes = static_cast<long>(packet.bytes.size()),
        .b_o_s = index == 0 ? 1 : 0,
        .e_o_s = eos ? 1 : 0,
        .granulepos = packet.granule == kOggUnknownGranule
                          ? ogg_int64_t{-1}
                          : static_cast<ogg_int64_t>(packet.granule),
        .packetno = static_cast<ogg_int64_t>(index),
    };
}

absl::StatusOr<std::uint64_t> ValidateVorbisPackets(
    const ParsedAudio& parsed) {
    VorbisState state;
    for (std::size_t index = 0; index < 3; ++index) {
        auto packet = VorbisPacket(parsed.packets[index], index, false);
        if (!packet.ok() || vorbis_synthesis_headerin(
                                &state.info, &state.comment, &*packet) < 0) {
            return absl::DataLossError("Invalid Vorbis header packet");
        }
    }
    if (vorbis_synthesis_init(&state.dsp, &state.info) != 0) {
        return absl::DataLossError("Invalid Vorbis synthesis setup");
    }
    state.dsp_initialized = true;
    if (vorbis_block_init(&state.dsp, &state.block) != 0) {
        return absl::ResourceExhaustedError(
            "Failed to allocate Vorbis synthesis block");
    }
    state.block_initialized = true;

    std::uint64_t decoded = 0;
    for (std::size_t index = 3; index < parsed.packets.size(); ++index) {
        auto packet = VorbisPacket(parsed.packets[index], index,
                                   index + 1 == parsed.packets.size());
        if (!packet.ok() || vorbis_synthesis(&state.block, &*packet) != 0 ||
            vorbis_synthesis_blockin(&state.dsp, &state.block) != 0) {
            return absl::DataLossError("Invalid Vorbis audio packet");
        }
        float** pcm = nullptr;
        for (;;) {
            const auto frames = vorbis_synthesis_pcmout(&state.dsp, &pcm);
            if (frames < 0) {
                return absl::DataLossError("Vorbis synthesis failed");
            }
            if (frames == 0) {
                break;
            }
            decoded += static_cast<std::uint64_t>(frames);
            if (vorbis_synthesis_read(&state.dsp, frames) != 0) {
                return absl::DataLossError("Vorbis synthesis read failed");
            }
        }
    }
    if (decoded == 0) {
        return absl::DataLossError("Vorbis stream has no effective PCM");
    }
    return decoded;
}

absl::Status InspectVorbis(ParsedAudio& parsed) {
    if (parsed.packets.size() < 4) {
        return absl::DataLossError("Vorbis stream lacks required packets");
    }
    const auto id = parsed.packets[0].bytes;
    if (id.size() != 30 || !Cmp(id.first<kVorbisId.size()>(), kVorbisId) ||
        ReadLe32(id.subspan(7, 4)) != 0 || (id[11] != 1 && id[11] != 2) ||
        ReadLe32(id.subspan(12, 4)) == 0 || (id[29] & 1U) == 0) {
        return absl::UnimplementedError("Unsupported Vorbis profile");
    }
    const auto small_block = id[28] & 0x0fU;
    const auto large_block = id[28] >> 4U;
    if (small_block < 6 || large_block > 13 || small_block > large_block) {
        return absl::DataLossError("Invalid Vorbis block sizes");
    }
    const auto& final_page = parsed.pages.back();
    if (!final_page.eos || final_page.granule == kOggUnknownGranule ||
        final_page.granule == 0) {
        return absl::DataLossError("Vorbis stream has no effective EOS");
    }
    parsed.facts.codec = AudioCodec::kVorbis;
    parsed.facts.channels = id[11];
    parsed.facts.sample_rate = ReadLe32(id.subspan(12, 4));
    parsed.final_granule = final_page.granule;
    for (std::size_t index = 3; index < parsed.packets.size(); ++index) {
        parsed.facts.audio_packet_bytes += parsed.packets[index].bytes.size();
    }
    auto decoded_frames = ValidateVorbisPackets(parsed);
    if (!decoded_frames.ok()) {
        return decoded_frames.status();
    }
    // A Vorbis logical stream may start at a positive PCM position. The final
    // granule is then an absolute position, not the number of frames emitted by
    // a decoder. libvorbis applies both start and end trimming while decoding.
    parsed.facts.frames = *decoded_frames;
    return absl::OkStatus();
}

absl::Status InspectParsed(ParsedAudio& parsed) {
    const auto first = parsed.packets.front().bytes;
    if (first.size() >= kOpusHead.size() &&
        Cmp(first.first<kOpusHead.size()>(), kOpusHead)) {
        return InspectOpus(parsed);
    }
    if (first.size() >= kVorbisId.size() &&
        Cmp(first.first<kVorbisId.size()>(), kVorbisId)) {
        return InspectVorbis(parsed);
    }
    return absl::UnimplementedError(
        "Ogg stream is not supported Opus or Vorbis");
}

BufferManaged CanonicalTags(const ParsedAudio& parsed, bool retain_r128) {
    constexpr std::string_view kVendor = "YSM";
    std::vector<std::string> comments;
    if (retain_r128 && parsed.r128_track.has_value()) {
        comments.push_back("R128_TRACK_GAIN=" +
                           std::to_string(*parsed.r128_track));
    }
    if (retain_r128 && parsed.r128_album.has_value()) {
        comments.push_back("R128_ALBUM_GAIN=" +
                           std::to_string(*parsed.r128_album));
    }

    std::vector<Byte> bytes(kOpusTags.begin(), kOpusTags.end());
    AppendLe32(bytes, static_cast<std::uint32_t>(kVendor.size()));
    bytes.insert(bytes.end(), kVendor.begin(), kVendor.end());
    AppendLe32(bytes, static_cast<std::uint32_t>(comments.size()));
    for (const auto& comment : comments) {
        AppendLe32(bytes, static_cast<std::uint32_t>(comment.size()));
        bytes.insert(bytes.end(), comment.begin(), comment.end());
    }
    return BufferManaged(BufferViewR(bytes));
}

absl::StatusOr<std::vector<OggPagePlan>> PlanPacketGroup(
    std::span<const OggMuxPacket> packets, std::size_t limit,
    std::uint32_t serial, std::uint32_t& sequence, bool bos, bool end_stream) {
    std::vector<OggPagePlan> pages;
    OggMuxCursor cursor;
    while (cursor.packet_index < packets.size()) {
        auto plan = PlanOggPage(packets, cursor, limit, serial, sequence++, bos,
                                end_stream);
        if (!plan.ok()) {
            return plan.status();
        }
        cursor = plan->next;
        pages.push_back(std::move(*plan));
        bos = false;
    }
    return pages;
}

struct PageGroup {
    std::span<const OggMuxPacket> packets;
    std::span<const OggPagePlan> pages;
};

absl::StatusOr<BufferManaged> WritePageGroups(
    std::span<const PageGroup> groups) {
    std::size_t size = 0;
    for (const auto& group : groups) {
        for (const auto& page : group.pages) {
            if (page.size() > BufferManaged::kMaxSize - size) {
                return absl::ResourceExhaustedError(
                    "Normalized Ogg chunk is too large");
            }
            size += page.size();
        }
    }
    BufferManaged result(size);
    BufferView destination = result;
    std::size_t offset = 0;
    for (const auto& group : groups) {
        for (const auto& page : group.pages) {
            auto status = WriteOggPage(
                page, group.packets,
                destination.subspan(offset, page.size()));
            if (!status.ok()) {
                return status;
            }
            offset += page.size();
        }
    }
    return result;
}

absl::Status DrainDecoder(OpusAudioStream& decoder, bool& produced) {
    std::array<Byte, 8 * 1024> pcm{};
    for (;;) {
        const auto result = decoder.Decode(pcm);
        if (result > 0) {
            produced = true;
            continue;
        }
        if (result == OpusAudioStream::kNeedInput || result == 0) {
            return absl::OkStatus();
        }
        return absl::DataLossError("Normalized Opus decode failed");
    }
}

absl::Status PackChunks(NormalizedAudio& audio,
                        std::span<const OggMuxPacket> tag_packets,
                        std::span<const OggPagePlan> tag_pages,
                        std::span<const OggMuxPacket> audio_packets,
                        std::span<const OggPagePlan> audio_pages,
                        std::uint64_t effective_pcm_start,
                        std::size_t page_limit) {
    std::size_t prefix_count = 0;
    while (prefix_count < audio_pages.size()) {
        const auto granule = audio_pages[prefix_count++].granule_position;
        if (granule != kOggUnknownGranule && granule > effective_pcm_start) {
            break;
        }
    }
    if (prefix_count == 0 || prefix_count > audio_pages.size() ||
        audio_pages[prefix_count - 1].granule_position == kOggUnknownGranule ||
        audio_pages[prefix_count - 1].granule_position <= effective_pcm_start) {
        return absl::DataLossError("Normalized Opus has no effective prefix");
    }

    const std::array prefix_groups{
        PageGroup{tag_packets, tag_pages},
        PageGroup{audio_packets, audio_pages.first(prefix_count)},
    };
    auto prefix = WritePageGroups(prefix_groups);
    if (!prefix.ok()) {
        return prefix.status();
    }
    audio.chunks.push_back(std::move(*prefix));

    std::size_t begin = prefix_count;
    while (begin < audio_pages.size()) {
        std::size_t end = begin;
        std::size_t size = 0;
        while (end < audio_pages.size() &&
               audio_pages[end].size() <= page_limit - size) {
            size += audio_pages[end].size();
            ++end;
        }
        if (end == begin) {
            return absl::InternalError("Normalized page exceeds chunk limit");
        }
        const std::array group{PageGroup{
            audio_packets, audio_pages.subspan(begin, end - begin)}};
        auto chunk = WritePageGroups(group);
        if (!chunk.ok()) {
            return chunk.status();
        }
        audio.chunks.push_back(std::move(*chunk));
        begin = end;
    }
    return absl::OkStatus();
}

absl::StatusOr<NormalizedAudio> NormalizeRetained(
    const ParsedAudio& parsed, const NormalizeAudioOptions& options) {
    if (parsed.facts.codec != AudioCodec::kOpus) {
        return absl::InvalidArgumentError(
            "Retained normalization requires Opus");
    }
    auto tags = CanonicalTags(parsed, true);
    const std::array head_packet{OggMuxPacket{parsed.packets[0].bytes, 0}};
    const std::array tag_packet{OggMuxPacket{BufferViewR(tags), 0}};

    std::vector<OggMuxPacket> audio_packets;
    audio_packets.reserve(parsed.packets.size() - 2);
    std::uint64_t decoded = 0;
    for (std::size_t index = 2; index < parsed.packets.size(); ++index) {
        const auto bytes = parsed.packets[index].bytes;
        const auto frames = opus_packet_get_nb_samples(
            bytes.data(), static_cast<opus_int32>(bytes.size()), 48'000);
        if (frames <= 0) {
            return absl::DataLossError("Invalid retained Opus packet");
        }
        decoded += static_cast<std::uint64_t>(frames);
        auto granule = parsed.origin + decoded;
        if (index + 1 == parsed.packets.size()) {
            granule = parsed.final_granule;
        }
        audio_packets.push_back(OggMuxPacket{bytes, granule});
    }

    std::uint32_t sequence = 0;
    auto head_pages =
        PlanPacketGroup(head_packet, kOggMaxPageSize,
                        parsed.facts.serial_number, sequence, true, false);
    if (!head_pages.ok() || head_pages->size() != 1) {
        return head_pages.ok()
                   ? absl::InternalError("OpusHead did not fit one page")
                   : head_pages.status();
    }
    auto tag_pages =
        PlanPacketGroup(tag_packet, options.page_limit,
                        parsed.facts.serial_number, sequence, false, false);
    if (!tag_pages.ok()) {
        return tag_pages.status();
    }
    auto audio_pages =
        PlanPacketGroup(audio_packets, options.page_limit,
                        parsed.facts.serial_number, sequence, false, true);
    if (!audio_pages.ok()) {
        return audio_pages.status();
    }

    const std::array head_group{
        PageGroup{head_packet, std::span(*head_pages)}};
    auto first_page = WritePageGroups(head_group);
    if (!first_page.ok()) {
        return first_page.status();
    }
    NormalizedAudio result{
        .facts = parsed.facts,
        .role = NormalizationRole::kRetained,
        .first_page = std::move(*first_page),
    };
    auto status = PackChunks(
        result, tag_packet, *tag_pages, audio_packets, *audio_pages,
        parsed.origin + parsed.facts.pre_skip, options.page_limit);
    if (!status.ok()) {
        return status;
    }
    return result;
}

class OpusEncodeSink {
   public:
    static absl::StatusOr<std::unique_ptr<OpusEncodeSink>> Create(
        std::uint64_t target_frames) {
        int error = OPUS_OK;
        auto* encoder = opus_encoder_create(kNormalizedSampleRate, 1,
                                            OPUS_APPLICATION_AUDIO, &error);
        if (encoder == nullptr || error != OPUS_OK) {
            if (encoder != nullptr) {
                opus_encoder_destroy(encoder);
            }
            return absl::ResourceExhaustedError(
                "Failed to allocate normalized Opus encoder");
        }
        auto result = std::unique_ptr<OpusEncodeSink>(
            new OpusEncodeSink(encoder, target_frames));
        int lookahead = 0;
        if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kOpusBitrate)) !=
                OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(0)) != OPUS_OK ||
            opus_encoder_ctl(encoder,
                             OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) !=
                OPUS_OK ||
            opus_encoder_ctl(encoder,
                             OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) !=
                OPUS_OK ||
            opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) !=
                OPUS_OK ||
            lookahead < 0 ||
            lookahead > (std::numeric_limits<std::uint16_t>::max)()) {
            return absl::FailedPreconditionError(
                "Failed to configure normalized Opus encoder");
        }
        result->pre_skip_ = static_cast<std::uint16_t>(lookahead);
        return result;
    }

    ~OpusEncodeSink() { opus_encoder_destroy(encoder_); }

    absl::Status Feed(std::span<const float> pcm, AudioStage& failure_stage) {
        if (signal_frames_ > target_frames_ ||
            pcm.size() > target_frames_ - signal_frames_) {
            failure_stage = AudioStage::kEncode;
            return absl::InternalError(
                "Transcoder produced excess normalized PCM");
        }
        signal_frames_ += pcm.size();
        return Push(pcm, failure_stage);
    }

    absl::Status Finish(AudioStage& failure_stage) {
        if (signal_frames_ != target_frames_) {
            failure_stage = AudioStage::kEncode;
            return absl::DataLossError(
                "Transcoder produced incomplete normalized PCM");
        }
        std::array<float, kOpusFrameSize> zeros{};
        std::size_t remaining = pre_skip_;
        while (remaining != 0) {
            const auto count = std::min(remaining, zeros.size());
            auto status = Push(std::span(zeros).first(count), failure_stage);
            if (!status.ok()) {
                return status;
            }
            remaining -= count;
        }
        if (frame_size_ != 0) {
            auto status =
                Push(std::span(zeros).first(kOpusFrameSize - frame_size_),
                     failure_stage);
            if (!status.ok()) {
                return status;
            }
        }
        if (packets_.empty()) {
            failure_stage = AudioStage::kEncode;
            return absl::DataLossError(
                "Normalized Opus encoder emitted no packets");
        }
        return absl::OkStatus();
    }

    [[nodiscard]] std::uint16_t pre_skip() const noexcept { return pre_skip_; }

    [[nodiscard]] std::uint64_t packet_bytes() const noexcept {
        return packet_bytes_;
    }

    [[nodiscard]] const std::vector<BufferManaged>& packets() const noexcept {
        return packets_;
    }

   private:
    OpusEncodeSink(OpusEncoder* encoder, std::uint64_t target_frames)
        : encoder_(encoder), target_frames_(target_frames) {}

    absl::Status Push(std::span<const float> pcm, AudioStage& failure_stage) {
        while (!pcm.empty()) {
            const auto count =
                std::min(pcm.size(), kOpusFrameSize - frame_size_);
            std::copy_n(pcm.begin(), count, frame_.begin() + frame_size_);
            frame_size_ += count;
            pcm = pcm.subspan(count);
            if (frame_size_ == kOpusFrameSize) {
                auto status = EncodeFrame(failure_stage);
                if (!status.ok()) {
                    return status;
                }
                frame_size_ = 0;
            }
        }
        return absl::OkStatus();
    }

    absl::Status EncodeFrame(AudioStage& failure_stage) {
        std::array<Byte, 4'000> packet{};
        const auto size = opus_encode_float(
            encoder_, frame_.data(), static_cast<int>(kOpusFrameSize),
            packet.data(), static_cast<opus_int32>(packet.size()));
        if (size <= 0) {
            failure_stage = AudioStage::kEncode;
            return absl::FailedPreconditionError(
                "Normalized Opus encoder rejected PCM");
        }
        packets_.emplace_back(
            BufferViewR(packet).first(static_cast<std::size_t>(size)));
        packet_bytes_ += static_cast<std::uint64_t>(size);
        return absl::OkStatus();
    }

    OpusEncoder* encoder_{};
    std::uint64_t target_frames_{};
    std::uint64_t signal_frames_{};
    std::uint64_t packet_bytes_{};
    std::uint16_t pre_skip_{};
    std::array<float, kOpusFrameSize> frame_{};
    std::size_t frame_size_{};
    std::vector<BufferManaged> packets_;
};

class ResamplePipe {
   public:
    static absl::StatusOr<std::unique_ptr<ResamplePipe>> Create(
        std::uint32_t source_rate, std::uint64_t target_frames,
        OpusEncodeSink& sink) {
        if (source_rate == 0) {
            return absl::InvalidArgumentError("Invalid source sample rate");
        }
        SpeexResamplerState* state = nullptr;
        if (source_rate != kNormalizedSampleRate) {
            int error = RESAMPLER_ERR_SUCCESS;
            state = speex_resampler_init(1, source_rate, kNormalizedSampleRate,
                                         5, &error);
            if (state == nullptr || error != RESAMPLER_ERR_SUCCESS) {
                if (state != nullptr) {
                    speex_resampler_destroy(state);
                }
                return absl::ResourceExhaustedError(
                    "Failed to allocate audio resampler");
            }
            if (speex_resampler_skip_zeros(state) != RESAMPLER_ERR_SUCCESS) {
                speex_resampler_destroy(state);
                return absl::FailedPreconditionError(
                    "Failed to initialize audio resampler");
            }
        }
        return std::unique_ptr<ResamplePipe>(
            new ResamplePipe(state, target_frames, sink));
    }

    ~ResamplePipe() {
        if (state_ != nullptr) {
            speex_resampler_destroy(state_);
        }
    }

    absl::Status Feed(std::span<const float> pcm, AudioStage& failure_stage) {
        if (produced_frames_ > target_frames_) {
            failure_stage = AudioStage::kResample;
            return absl::InternalError(
                "Audio resampler exceeded the target timeline");
        }
        if (state_ == nullptr) {
            const auto count = std::min<std::uint64_t>(
                pcm.size(), target_frames_ - produced_frames_);
            auto status = sink_.Feed(pcm.first(static_cast<std::size_t>(count)),
                                     failure_stage);
            if (!status.ok()) {
                return status;
            }
            produced_frames_ += count;
            return absl::OkStatus();
        }
        while (!pcm.empty() && produced_frames_ < target_frames_) {
            auto input_size = static_cast<spx_uint32_t>(std::min<std::size_t>(
                pcm.size(), (std::numeric_limits<spx_uint32_t>::max)()));
            auto output_size =
                static_cast<spx_uint32_t>(std::min<std::uint64_t>(
                    output_.size(), target_frames_ - produced_frames_));
            const auto error = speex_resampler_process_float(
                state_, 0, pcm.data(), &input_size, output_.data(),
                &output_size);
            if (error != RESAMPLER_ERR_SUCCESS ||
                (input_size == 0 && output_size == 0)) {
                failure_stage = AudioStage::kResample;
                return absl::FailedPreconditionError(
                    "Audio resampler made no progress");
            }
            auto status = sink_.Feed(std::span(output_).first(output_size),
                                     failure_stage);
            if (!status.ok()) {
                return status;
            }
            produced_frames_ += output_size;
            pcm = pcm.subspan(input_size);
        }
        if (!pcm.empty()) {
            failure_stage = AudioStage::kResample;
            return absl::DataLossError(
                "Audio resampler reached the target before consuming PCM");
        }
        return absl::OkStatus();
    }

    absl::Status Finish(AudioStage& failure_stage) {
        if (state_ == nullptr) {
            if (produced_frames_ != target_frames_) {
                failure_stage = AudioStage::kResample;
                return absl::DataLossError(
                    "Source PCM frame count is incomplete");
            }
            return absl::OkStatus();
        }
        std::array<float, 1'024> zeros{};
        std::size_t empty_iterations = 0;
        while (produced_frames_ < target_frames_) {
            auto input_size = static_cast<spx_uint32_t>(zeros.size());
            auto output_size =
                static_cast<spx_uint32_t>(std::min<std::uint64_t>(
                    output_.size(), target_frames_ - produced_frames_));
            const auto error = speex_resampler_process_float(
                state_, 0, zeros.data(), &input_size, output_.data(),
                &output_size);
            if (error != RESAMPLER_ERR_SUCCESS ||
                (input_size == 0 && output_size == 0) ||
                (output_size == 0 && ++empty_iterations > 32)) {
                failure_stage = AudioStage::kResample;
                return absl::FailedPreconditionError(
                    "Audio resampler failed to drain");
            }
            if (output_size != 0) {
                empty_iterations = 0;
            }
            auto status = sink_.Feed(std::span(output_).first(output_size),
                                     failure_stage);
            if (!status.ok()) {
                return status;
            }
            produced_frames_ += output_size;
        }
        return absl::OkStatus();
    }

   private:
    ResamplePipe(SpeexResamplerState* state, std::uint64_t target_frames,
                 OpusEncodeSink& sink)
        : state_(state), target_frames_(target_frames), sink_(sink) {}

    SpeexResamplerState* state_{};
    std::uint64_t target_frames_{};
    std::uint64_t produced_frames_{};
    OpusEncodeSink& sink_;
    std::array<float, 4'096> output_{};
};

absl::StatusOr<std::uint64_t> ResampledFrameCount(std::uint64_t frames,
                                                  std::uint32_t rate) {
    if (rate == 0) {
        return absl::InvalidArgumentError("Invalid source sample rate");
    }
    const auto quotient = frames / rate;
    const auto remainder = frames % rate;
    if (quotient >
        (std::numeric_limits<std::uint64_t>::max)() / kNormalizedSampleRate) {
        return absl::ResourceExhaustedError("Resampled frame count overflows");
    }
    const auto rounded_remainder =
        (remainder * kNormalizedSampleRate + rate / 2U) / rate;
    const auto result = quotient * kNormalizedSampleRate + rounded_remainder;
    if (result == 0) {
        return absl::DataLossError("Audio has no resampled PCM");
    }
    return result;
}

absl::Status DecodeOpus(const ParsedAudio& parsed, ResamplePipe& output,
                        AudioStage& failure_stage,
                        absl::FunctionRef<bool()> cancelled) {
    int error = OPUS_OK;
    auto* decoder = opus_decoder_create(kNormalizedSampleRate,
                                        parsed.facts.channels, &error);
    if (decoder == nullptr || error != OPUS_OK) {
        if (decoder != nullptr) {
            opus_decoder_destroy(decoder);
        }
        failure_stage = AudioStage::kDecode;
        return absl::ResourceExhaustedError(
            "Failed to allocate Opus transcode decoder");
    }
    ScopeGuard close([decoder] { opus_decoder_destroy(decoder); });
    std::array<float, 5'760 * 2> pcm{};
    std::array<float, 5'760> mono{};
    const auto valid_begin = static_cast<std::uint64_t>(parsed.facts.pre_skip);
    const auto valid_end = valid_begin + parsed.facts.frames;
    std::uint64_t decoded = 0;
    std::uint64_t emitted = 0;
    for (std::size_t index = 2; index < parsed.packets.size(); ++index) {
        if (cancelled()) {
            failure_stage = AudioStage::kDecode;
            return absl::CancelledError("Audio normalization cancelled");
        }
        const auto packet = parsed.packets[index].bytes;
        const auto frames = opus_decode_float(
            decoder, packet.data(), static_cast<opus_int32>(packet.size()),
            pcm.data(), 5'760, 0);
        if (frames <= 0) {
            failure_stage = AudioStage::kDecode;
            return absl::DataLossError("Opus transcode decode failed");
        }
        const auto packet_end = decoded + static_cast<std::uint64_t>(frames);
        const auto begin = std::max(decoded, valid_begin);
        const auto end = std::min(packet_end, valid_end);
        if (begin < end) {
            const auto offset = static_cast<std::size_t>(begin - decoded);
            const auto count = static_cast<std::size_t>(end - begin);
            std::span<const float> samples;
            if (parsed.facts.channels == 1) {
                samples = std::span(pcm).subspan(offset, count);
            } else {
                for (std::size_t frame = 0; frame < count; ++frame) {
                    mono[frame] = (pcm[(offset + frame) * 2] +
                                   pcm[(offset + frame) * 2 + 1]) *
                                  0.5F;
                }
                samples = std::span(mono).first(count);
            }
            auto status = output.Feed(samples, failure_stage);
            if (!status.ok()) {
                return status;
            }
            emitted += count;
        }
        decoded = packet_end;
    }
    if (emitted != parsed.facts.frames) {
        failure_stage = AudioStage::kDecode;
        return absl::DataLossError("Opus timeline did not yield exact PCM");
    }
    return absl::OkStatus();
}

absl::Status DecodeVorbis(const ParsedAudio& parsed, ResamplePipe& output,
                          AudioStage& failure_stage,
                          absl::FunctionRef<bool()> cancelled) {
    VorbisState state;
    for (std::size_t index = 0; index < 3; ++index) {
        auto packet = VorbisPacket(parsed.packets[index], index, false);
        if (!packet.ok() || vorbis_synthesis_headerin(
                                &state.info, &state.comment, &*packet) < 0) {
            failure_stage = AudioStage::kDecode;
            return absl::DataLossError("Invalid Vorbis transcode header");
        }
    }
    if (vorbis_synthesis_init(&state.dsp, &state.info) != 0) {
        failure_stage = AudioStage::kDecode;
        return absl::DataLossError("Invalid Vorbis transcode setup");
    }
    state.dsp_initialized = true;
    if (vorbis_block_init(&state.dsp, &state.block) != 0) {
        failure_stage = AudioStage::kDecode;
        return absl::ResourceExhaustedError(
            "Failed to allocate Vorbis transcode block");
    }
    state.block_initialized = true;

    std::array<float, 4'096> mono{};
    std::uint64_t emitted = 0;
    for (std::size_t index = 3; index < parsed.packets.size(); ++index) {
        if (cancelled()) {
            failure_stage = AudioStage::kDecode;
            return absl::CancelledError("Audio normalization cancelled");
        }
        auto packet = VorbisPacket(parsed.packets[index], index,
                                   index + 1 == parsed.packets.size());
        if (!packet.ok() || vorbis_synthesis(&state.block, &*packet) != 0 ||
            vorbis_synthesis_blockin(&state.dsp, &state.block) != 0) {
            failure_stage = AudioStage::kDecode;
            return absl::DataLossError("Vorbis transcode decode failed");
        }
        float** pcm = nullptr;
        for (;;) {
            const auto available = vorbis_synthesis_pcmout(&state.dsp, &pcm);
            if (available < 0) {
                failure_stage = AudioStage::kDecode;
                return absl::DataLossError("Vorbis PCM extraction failed");
            }
            if (available == 0) {
                break;
            }
            std::size_t consumed = 0;
            const auto admitted =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    available, parsed.facts.frames - emitted));
            while (consumed < admitted) {
                const auto count = std::min(mono.size(), admitted - consumed);
                if (parsed.facts.channels == 1) {
                    std::copy_n(pcm[0] + consumed, count, mono.begin());
                } else {
                    for (std::size_t frame = 0; frame < count; ++frame) {
                        mono[frame] = (pcm[0][consumed + frame] +
                                       pcm[1][consumed + frame]) *
                                      0.5F;
                    }
                }
                auto status =
                    output.Feed(std::span(mono).first(count), failure_stage);
                if (!status.ok()) {
                    return status;
                }
                consumed += count;
                emitted += count;
            }
            if (vorbis_synthesis_read(&state.dsp, available) != 0) {
                failure_stage = AudioStage::kDecode;
                return absl::DataLossError("Vorbis PCM consume failed");
            }
        }
    }
    if (emitted != parsed.facts.frames) {
        failure_stage = AudioStage::kDecode;
        return absl::DataLossError("Vorbis timeline did not yield exact PCM");
    }
    return absl::OkStatus();
}

std::uint32_t StableSerial(BufferViewR source) noexcept {
    std::uint32_t hash = 2'166'136'261U;
    for (const auto byte : source) {
        hash = (hash ^ byte) * 16'777'619U;
    }
    hash ^= 0x54534d59U;
    return hash == 0 ? 1 : hash;
}

BufferManaged TranscodedHead(std::uint16_t pre_skip, std::int16_t output_gain) {
    std::vector<Byte> bytes(kOpusHead.begin(), kOpusHead.end());
    bytes.push_back(1);
    bytes.push_back(1);
    AppendLe16(bytes, pre_skip);
    AppendLe32(bytes, kNormalizedSampleRate);
    AppendLe16(bytes, static_cast<std::uint16_t>(output_gain));
    bytes.push_back(0);
    return BufferManaged(BufferViewR(bytes));
}

absl::StatusOr<NormalizedAudio> NormalizeTranscoded(
    BufferViewR source, const ParsedAudio& parsed,
    const NormalizeAudioOptions& options, AudioStage& failure_stage,
    absl::FunctionRef<bool()> cancelled) {
    auto target_frames =
        ResampledFrameCount(parsed.facts.frames, parsed.facts.sample_rate);
    if (!target_frames.ok()) {
        failure_stage = AudioStage::kResample;
        return target_frames.status();
    }
    const auto output_gain = parsed.facts.codec == AudioCodec::kOpus
                                 ? parsed.facts.output_gain
                                 : std::int16_t{0};
    auto encoder = OpusEncodeSink::Create(*target_frames);
    if (!encoder.ok()) {
        failure_stage = AudioStage::kEncode;
        return encoder.status();
    }
    auto resampler = ResamplePipe::Create(parsed.facts.sample_rate,
                                          *target_frames, **encoder);
    if (!resampler.ok()) {
        failure_stage = AudioStage::kResample;
        return resampler.status();
    }
    failure_stage = AudioStage::kDecode;
    auto status =
        parsed.facts.codec == AudioCodec::kOpus
            ? DecodeOpus(parsed, **resampler, failure_stage, cancelled)
            : DecodeVorbis(parsed, **resampler, failure_stage, cancelled);
    if (!status.ok()) {
        return status;
    }
    status = (*resampler)->Finish(failure_stage);
    if (!status.ok()) {
        return status;
    }
    status = (*encoder)->Finish(failure_stage);
    if (!status.ok()) {
        return status;
    }

    const auto serial = options.transcoded_serial == 0
                            ? StableSerial(source)
                            : options.transcoded_serial;
    auto head = TranscodedHead((*encoder)->pre_skip(), output_gain);
    auto tags = CanonicalTags(parsed, false);
    const std::array head_packet{OggMuxPacket{BufferViewR(head), 0}};
    const std::array tag_packet{OggMuxPacket{BufferViewR(tags), 0}};
    std::vector<OggMuxPacket> audio_packets;
    audio_packets.reserve((*encoder)->packets().size());
    std::uint64_t decoded = 0;
    for (std::size_t index = 0; index < (*encoder)->packets().size(); ++index) {
        decoded += kOpusFrameSize;
        auto granule = decoded;
        if (index + 1 == (*encoder)->packets().size()) {
            granule = (*encoder)->pre_skip() + *target_frames;
        }
        audio_packets.push_back(
            OggMuxPacket{BufferViewR((*encoder)->packets()[index]), granule});
    }

    failure_stage = AudioStage::kMux;
    std::uint32_t sequence = 0;
    auto head_pages = PlanPacketGroup(head_packet, kOggMaxPageSize, serial,
                                      sequence, true, false);
    if (!head_pages.ok() || head_pages->size() != 1) {
        return head_pages.ok()
                   ? absl::InternalError("OpusHead did not fit one page")
                   : head_pages.status();
    }
    auto tag_pages = PlanPacketGroup(tag_packet, options.page_limit, serial,
                                     sequence, false, false);
    if (!tag_pages.ok()) {
        return tag_pages.status();
    }
    auto encoded_pages = PlanPacketGroup(audio_packets, options.page_limit,
                                         serial, sequence, false, true);
    if (!encoded_pages.ok()) {
        return encoded_pages.status();
    }

    const std::array head_group{
        PageGroup{head_packet, std::span(*head_pages)}};
    auto first_page = WritePageGroups(head_group);
    if (!first_page.ok()) {
        return first_page.status();
    }
    NormalizedAudio result{
        .facts =
            AudioFacts{
                .codec = AudioCodec::kOpus,
                .channels = 1,
                .sample_rate = kNormalizedSampleRate,
                .frames = *target_frames,
                .audio_packet_bytes = (*encoder)->packet_bytes(),
                .serial_number = serial,
                .pre_skip = (*encoder)->pre_skip(),
                .output_gain = output_gain,
            },
        .role = NormalizationRole::kTranscoded,
        .first_page = std::move(*first_page),
    };
    status = PackChunks(result, tag_packet, *tag_pages, audio_packets,
                        *encoded_pages, (*encoder)->pre_skip(),
                        options.page_limit);
    if (!status.ok()) {
        return status;
    }
    return result;
}

AudioFailureKind FailureKind(const absl::Status& status,
                             NormalizationRole role) {
    if (absl::IsCancelled(status)) {
        return AudioFailureKind::kCancelled;
    }
    if (absl::IsUnimplemented(status)) {
        return AudioFailureKind::kUnsupportedProfile;
    }
    if (absl::IsDataLoss(status) || absl::IsInvalidArgument(status)) {
        return AudioFailureKind::kSourceInvalid;
    }
    if (role == NormalizationRole::kTranscoded &&
        absl::IsFailedPrecondition(status)) {
        return AudioFailureKind::kOptionalCandidateFailure;
    }
    return AudioFailureKind::kEnvironmentFailure;
}

NormalizeAudioResult FailResult(const absl::Status& status,
                                NormalizationRole role, AudioStage stage) {
    return NormalizeAudioResult::Failure(
        AudioFailure{FailureKind(status, role), stage, status});
}
}  // namespace

NormalizeAudioResult NormalizeAudioResult::Success(NormalizedAudio value) {
    NormalizeAudioResult result;
    result.value_.emplace(std::move(value));
    return result;
}

NormalizeAudioResult NormalizeAudioResult::Failure(AudioFailure failure) {
    NormalizeAudioResult result;
    result.failure_ = std::move(failure);
    return result;
}

absl::StatusOr<AudioFacts> InspectAudio(BufferViewR source) {
    auto parsed = ParseContainer(source);
    if (!parsed.ok()) {
        return parsed.status();
    }
    auto status = InspectParsed(*parsed);
    if (!status.ok()) {
        return status;
    }
    return parsed->facts;
}

NormalizeAudioResult NormalizeAudio(BufferViewR source,
                                    const NormalizeAudioOptions& options,
                                    absl::FunctionRef<bool()> cancelled) {
    if (cancelled()) {
        return FailResult(absl::CancelledError("Audio normalization cancelled"),
                          options.role, AudioStage::kInspect);
    }
    if (options.page_limit < kOggPageHeaderSize + 1 ||
        options.page_limit > 57'344) {
        return FailResult(
            absl::InvalidArgumentError("Invalid normalized Ogg page limit"),
            options.role, AudioStage::kMux);
    }
    auto parsed = ParseContainer(source);
    if (!parsed.ok()) {
        return FailResult(parsed.status(), options.role, AudioStage::kInspect);
    }
    auto inspect = InspectParsed(*parsed);
    if (!inspect.ok()) {
        return FailResult(inspect, options.role, AudioStage::kInspect);
    }
    if (cancelled()) {
        return FailResult(absl::CancelledError("Audio normalization cancelled"),
                          options.role, AudioStage::kInspect);
    }
    try {
        auto failure_stage = options.role == NormalizationRole::kTranscoded
                                 ? AudioStage::kDecode
                                 : AudioStage::kMux;
        auto normalized = options.role == NormalizationRole::kTranscoded
                              ? NormalizeTranscoded(source, *parsed, options,
                                                    failure_stage, cancelled)
                              : NormalizeRetained(*parsed, options);
        if (!normalized.ok()) {
            return FailResult(normalized.status(), options.role, failure_stage);
        }
        auto validation = ValidateComplete(*normalized);
        if (!validation.ok()) {
            return FailResult(validation, options.role, AudioStage::kValidate);
        }
        return NormalizeAudioResult::Success(std::move(*normalized));
    } catch (const std::bad_alloc&) {
        return FailResult(absl::ResourceExhaustedError(
                              "Audio normalization allocation failed"),
                          options.role, AudioStage::kMux);
    }
}

absl::Status ValidatePrefix(const NormalizedAudio& audio) {
    if (audio.first_page.empty() || audio.chunks.empty()) {
        return absl::InvalidArgumentError(
            "Normalized audio prefix is incomplete");
    }
    OpusAudioStream decoder(audio.facts.frames);
    auto head = decoder.Feed(audio.first_page);
    if (!head.ok() || head->consumed != audio.first_page.size()) {
        return absl::DataLossError("Invalid normalized OpusHead page");
    }
    std::size_t offset = 0;
    const auto& first = audio.chunks.front();
    while (offset < first.size()) {
        auto feed = decoder.Feed(BufferViewR(first).subspan(offset));
        if (!feed.ok() || feed->consumed == 0) {
            return feed.ok() ? absl::DataLossError(
                                   "Normalized Opus prefix made no progress")
                             : feed.status();
        }
        offset += feed->consumed;
        bool produced = false;
        auto status = DrainDecoder(decoder, produced);
        if (!status.ok()) {
            return status;
        }
        if (produced) {
            return absl::OkStatus();
        }
    }
    return absl::DataLossError(
        "Normalized Opus prefix produced no effective PCM");
}

absl::Status ValidateComplete(const NormalizedAudio& audio) {
    auto prefix = ValidatePrefix(audio);
    if (!prefix.ok()) {
        return prefix;
    }
    OpusAudioStream decoder(audio.facts.frames);
    std::uint64_t pcm_frames = 0;
    const auto feed_all = [&](BufferViewR input) -> absl::Status {
        std::size_t offset = 0;
        while (offset < input.size()) {
            auto result = decoder.Feed(input.subspan(offset));
            if (!result.ok() || result->consumed == 0) {
                return result.ok()
                           ? absl::DataLossError(
                                 "Normalized Opus feed made no progress")
                           : result.status();
            }
            offset += result->consumed;
            std::array<Byte, 8 * 1024> pcm{};
            for (;;) {
                const auto decoded = decoder.Decode(pcm);
                if (decoded > 0) {
                    pcm_frames += static_cast<std::uint64_t>(decoded) /
                                  sizeof(std::int16_t);
                } else if (decoded == OpusAudioStream::kNeedInput) {
                    break;
                } else {
                    return absl::DataLossError(
                        "Normalized Opus failed before input EOF");
                }
            }
        }
        return absl::OkStatus();
    };
    auto status = feed_all(audio.first_page);
    if (!status.ok()) {
        return status;
    }
    for (const auto& chunk : audio.chunks) {
        status = feed_all(chunk);
        if (!status.ok()) {
            return status;
        }
    }
    decoder.EndInput();
    std::array<Byte, 8 * 1024> pcm{};
    for (;;) {
        const auto decoded = decoder.Decode(pcm);
        if (decoded > 0) {
            pcm_frames +=
                static_cast<std::uint64_t>(decoded) / sizeof(std::int16_t);
        } else if (decoded == 0) {
            break;
        } else {
            return absl::DataLossError(
                "Normalized Opus did not reach verified EOF");
        }
    }
    return pcm_frames == audio.facts.frames
               ? absl::OkStatus()
               : absl::DataLossError(
                     "Normalized Opus PCM frame count mismatch");
}
}  // namespace ysm::codec
