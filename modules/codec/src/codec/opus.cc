#include "codec/opus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include <absl/status/status.h>
#include <opus/opus.h>

#include "buffer_managed.h"
#include "codec/mini_ogg.h"

namespace ysm::codec {
namespace {
constexpr std::int32_t kSampleRate = 48'000;
constexpr std::size_t kMaxPacketFrames = 5'760;
constexpr std::array<Byte, 8> kOpusHead{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
constexpr std::array<Byte, 8> kOpusTags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};

std::uint16_t ReadLe16(BufferViewR bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1]) << 8U;
}

struct PacketRef {
    BufferViewR bytes;
};

struct OpusStreamState {
    enum class HeaderState : std::uint8_t {
        kHead,
        kTags,
        kAudio,
    };

    std::uint64_t expected_frames_{};
    std::uint64_t decoded_frames_{};
    std::uint64_t emitted_frames_{};
    std::uint64_t origin_{};
    std::uint64_t previous_granule_{};
    std::uint32_t serial_number_{};
    std::uint32_t next_sequence_{};
    std::uint32_t pre_skip_{};
    std::uint8_t channels_{};
    HeaderState header_state_{HeaderState::kHead};
    bool has_stream_{};
    bool has_origin_{};
    bool has_granule_{};
    bool eos_seen_{};
    bool input_ended_{};
    bool failed_{};
};
}  // namespace

class OpusAudioCheckpoint::Impl : public OpusStreamState {
   public:
    BufferManaged decoder_state_;
    BufferManaged packet_buffer_;
};

YSM_PIMPL_DEFINITION(OpusAudioCheckpoint)

OpusAudioCheckpoint::OpusAudioCheckpoint() = default;
OpusAudioCheckpoint::~OpusAudioCheckpoint() = default;

std::size_t OpusAudioCheckpoint::OwnedBytes() const noexcept {
    return sizeof(*this) + Pimpl().decoder_state_.capacity() +
           Pimpl().packet_buffer_.capacity();
}

std::uint64_t OpusAudioCheckpoint::EmittedFrames() const noexcept {
    return Pimpl().emitted_frames_;
}

class OpusAudioStream::Impl : public OpusStreamState {

    OpusDecoder* decoder_{};
    BufferManaged page_buffer_;
    BufferManaged packet_buffer_;
    BufferManaged pcm_buffer_;
    std::size_t pcm_position_{};

   public:
    explicit Impl(std::uint64_t expected_frames) {
        expected_frames_ = expected_frames;
        int error = OPUS_OK;
        decoder_ = opus_decoder_create(kSampleRate, 2, &error);
        if (decoder_ == nullptr || error != OPUS_OK) {
            if (decoder_ != nullptr) {
                opus_decoder_destroy(decoder_);
            }
            throw std::bad_alloc();
        }
    }

    ~Impl() { opus_decoder_destroy(decoder_); }

    explicit Impl(const OpusAudioCheckpoint::Impl& checkpoint)
        : OpusStreamState(checkpoint),
          packet_buffer_(checkpoint.packet_buffer_) {
        int error = OPUS_OK;
        decoder_ = opus_decoder_create(kSampleRate, channels_, &error);
        if (decoder_ == nullptr || error != OPUS_OK) {
            opus_decoder_destroy(decoder_);
            throw std::bad_alloc();
        }
        // libopus states are contiguous and support memcpy. The allocation and
        // copied length must both use the source channel mode.
        std::memcpy(decoder_, checkpoint.decoder_state_.data(),
                    checkpoint.decoder_state_.size());
    }

    absl::StatusOr<std::size_t> CheckpointSize() const {
        if (failed_ || input_ended_ || eos_seen_ ||
            header_state_ != HeaderState::kAudio || !page_buffer_.empty() ||
            !pcm_buffer_.empty() || pcm_position_ != 0) {
            return absl::FailedPreconditionError(
                "Opus checkpoint requires a live, drained whole-page boundary");
        }
        return sizeof(OpusAudioCheckpoint) +
               static_cast<std::size_t>(opus_decoder_get_size(channels_)) +
               packet_buffer_.size();
    }

    void Capture(OpusAudioCheckpoint::Impl& checkpoint) const {
        static_cast<OpusStreamState&>(checkpoint) = *this;
        checkpoint.decoder_state_.CopyFrom(
            {reinterpret_cast<const Byte*>(decoder_),
             static_cast<std::size_t>(opus_decoder_get_size(channels_))});
        checkpoint.packet_buffer_.CopyFrom(packet_buffer_);
    }

    absl::StatusOr<OpusFeedResult> Feed(BufferViewR input) {
        if (failed_) {
            return absl::FailedPreconditionError("Opus decoder has failed");
        }
        if (input_ended_) {
            return FailFeed(absl::FailedPreconditionError(
                "Opus input is already terminal"));
        }
        if (eos_seen_ && !input.empty()) {
            return FailFeed(
                absl::DataLossError("Trailing bytes after Ogg EOS"));
        }
        if (pcm_position_ < pcm_buffer_.size()) {
            return OpusFeedResult{0, OpusFeedStatus::kOutputReady};
        }
        pcm_buffer_.clear();
        pcm_position_ = 0;

        std::size_t consumed = 0;
        while (consumed < input.size() || !page_buffer_.empty()) {
            const bool staged = !page_buffer_.empty();
            const auto page = AcquirePage(input.subspan(consumed));
            if (!page.ok()) {
                if (absl::IsOutOfRange(page.status())) {
                    if (consumed == input.size()) {
                        return OpusFeedResult{consumed,
                                              OpusFeedStatus::kNeedInput};
                    }
                    const auto copied =
                        CopyPageFragment(input.subspan(consumed));
                    if (!copied.ok()) {
                        return FailFeed(copied.status());
                    }
                    consumed += *copied;
                    continue;
                }
                return FailFeed(page.status());
            }

            if (!staged) {
                consumed += page->bytes.size();
            }
            const auto status = ProcessPage(*page);
            if (!status.ok()) {
                return FailFeed(status);
            }
            if (staged) {
                page_buffer_.clear();
            }
            if (!pcm_buffer_.empty()) {
                return OpusFeedResult{consumed, OpusFeedStatus::kOutputReady};
            }
            if (eos_seen_) {
                if (consumed != input.size()) {
                    return FailFeed(
                        absl::DataLossError("Trailing bytes after Ogg EOS"));
                }
                break;
            }
        }
        return OpusFeedResult{consumed, consumed == 0
                                            ? OpusFeedStatus::kNeedInput
                                            : OpusFeedStatus::kProgress};
    }

    void EndInput() noexcept { input_ended_ = true; }

    std::int32_t Decode(BufferView destination) {
        if (failed_) {
            return kError;
        }
        destination =
            destination.first(destination.size() - destination.size() % 2);
        if (destination.empty()) {
            failed_ = true;
            return kError;
        }
        if (pcm_position_ < pcm_buffer_.size()) {
            const auto size = std::min(destination.size(),
                                       pcm_buffer_.size() - pcm_position_);
            std::memcpy(destination.data(), pcm_buffer_.data() + pcm_position_,
                        size);
            pcm_position_ += size;
            if (pcm_position_ == pcm_buffer_.size()) {
                pcm_buffer_.clear();
                pcm_position_ = 0;
            }
            return static_cast<std::int32_t>(size);
        }
        if (!input_ended_) {
            return kNeedInput;
        }
        if (page_buffer_.empty() && packet_buffer_.empty() && eos_seen_ &&
            header_state_ == HeaderState::kAudio &&
            emitted_frames_ == expected_frames_) {
            return 0;
        }
        failed_ = true;
        return kError;
    }

   private:
    absl::StatusOr<OggPageView> AcquirePage(BufferViewR input) {
        if (!page_buffer_.empty()) {
            return ReadOggPage(page_buffer_);
        }
        return ReadOggPage(input);
    }

    absl::StatusOr<std::size_t> CopyPageFragment(BufferViewR input) {
        if (input.empty()) {
            return std::size_t{0};
        }
        std::size_t copy_size = 0;
        while (copy_size < input.size()) {
            const auto required = RequiredPageSize(page_buffer_);
            if (required == 0) {
                return absl::DataLossError("Invalid staged Ogg page");
            }
            if (page_buffer_.size() == required) {
                if (required >= kOggPageHeaderSize) {
                    const auto page = ReadOggPage(page_buffer_);
                    if (page.ok()) {
                        break;
                    }
                    if (!absl::IsOutOfRange(page.status())) {
                        return page.status();
                    }
                }
                continue;
            }
            const auto amount = std::min(input.size() - copy_size,
                                         required - page_buffer_.size());
            Append(page_buffer_, input.subspan(copy_size, amount));
            copy_size += amount;
        }
        return copy_size;
    }

    static std::size_t RequiredPageSize(BufferViewR prefix) noexcept {
        if (prefix.size() < kOggPageHeaderSize) {
            return kOggPageHeaderSize;
        }
        if (prefix[0] != 'O' || prefix[1] != 'g' || prefix[2] != 'g' ||
            prefix[3] != 'S') {
            return 0;
        }
        const auto segment_count = static_cast<std::size_t>(prefix[26]);
        const auto header_size = kOggPageHeaderSize + segment_count;
        if (prefix.size() < header_size) {
            return header_size;
        }
        std::size_t body_size = 0;
        for (const auto value :
             prefix.subspan(kOggPageHeaderSize, segment_count)) {
            body_size += value;
        }
        return header_size + body_size;
    }

    static void Append(BufferManaged& destination, BufferViewR source) {
        if (source.size() > kOggMaxPageSize - destination.size()) {
            throw std::bad_alloc();
        }
        const auto offset = destination.size();
        destination.resize(offset + source.size());
        std::memcpy(destination.data() + offset, source.data(), source.size());
    }

    absl::Status ProcessPage(const OggPageView& page) {
        if (!has_stream_) {
            if (!page.bos || page.continuation || page.sequence != 0) {
                return absl::DataLossError("Invalid first Ogg page");
            }
            has_stream_ = true;
            serial_number_ = page.serial_number;
        } else if (page.bos || page.serial_number != serial_number_ ||
                   page.sequence != next_sequence_) {
            return absl::DataLossError("Discontinuous Ogg logical stream");
        }
        ++next_sequence_;
        if (page.continuation != !packet_buffer_.empty()) {
            return absl::DataLossError("Invalid Ogg packet continuation");
        }
        if (eos_seen_) {
            return absl::DataLossError("Ogg page follows EOS");
        }

        std::array<PacketRef, kOggMaxSegments> packets{};
        std::size_t packet_count = 0;
        BufferManaged completed_continuation;
        std::size_t body_offset = 0;
        std::size_t packet_start = 0;
        bool assembling = !packet_buffer_.empty();

        for (std::size_t index = 0; index < page.lacing.size(); ++index) {
            const auto size = static_cast<std::size_t>(page.lacing[index]);
            if (body_offset > page.body.size() ||
                size > page.body.size() - body_offset) {
                return absl::DataLossError("Invalid Ogg lacing");
            }
            if (assembling) {
                Append(packet_buffer_, page.body.subspan(body_offset, size));
            }
            body_offset += size;
            if (size == 255) {
                continue;
            }

            if (assembling) {
                completed_continuation = std::move(packet_buffer_);
                packets[packet_count++].bytes = completed_continuation;
                packet_buffer_ = BufferManaged();
                assembling = false;
            } else {
                packets[packet_count++].bytes =
                    page.body.subspan(packet_start, body_offset - packet_start);
            }
            packet_start = body_offset;
        }
        if (body_offset != page.body.size()) {
            return absl::DataLossError("Ogg body has unreferenced bytes");
        }
        if (!page.lacing.empty() && page.lacing.back() == 255) {
            if (!assembling) {
                Append(packet_buffer_,
                       page.body.subspan(packet_start,
                                         body_offset - packet_start));
            }
        }
        if (page.eos && !packet_buffer_.empty()) {
            return absl::DataLossError("Ogg EOS has an unfinished packet");
        }

        std::size_t first_audio = packet_count;
        for (std::size_t index = 0; index < packet_count; ++index) {
            if (header_state_ == HeaderState::kHead) {
                const auto status = ReadHead(packets[index].bytes);
                if (!status.ok()) {
                    return status;
                }
            } else if (header_state_ == HeaderState::kTags) {
                if (packets[index].bytes.size() < kOpusTags.size() ||
                    !Cmp(packets[index].bytes.first<kOpusTags.size()>(),
                         kOpusTags)) {
                    return absl::DataLossError("Invalid OpusTags packet");
                }
                header_state_ = HeaderState::kAudio;
            } else {
                first_audio = index;
                break;
            }
        }

        const auto status = ProcessAudioPackets(
            std::span(packets).subspan(first_audio, packet_count - first_audio),
            page);
        if (!status.ok()) {
            return status;
        }
        if (page.eos) {
            if (header_state_ != HeaderState::kAudio ||
                page.granule_position == kOggUnknownGranule) {
                return absl::DataLossError("Invalid Opus EOS");
            }
            eos_seen_ = true;
        }
        return absl::OkStatus();
    }

    absl::Status ReadHead(BufferViewR packet) {
        if (packet.size() != 19 ||
            !Cmp(packet.first<kOpusHead.size()>(), kOpusHead) ||
            packet[8] > 15 || (packet[9] != 1 && packet[9] != 2) ||
            packet[18] != 0) {
            return absl::DataLossError("Unsupported OpusHead");
        }
        channels_ = packet[9];
        pre_skip_ = ReadLe16(packet.subspan(10, 2));
        const auto gain =
            static_cast<std::int16_t>(ReadLe16(packet.subspan(16, 2)));
        if (opus_decoder_init(decoder_, kSampleRate, channels_) != OPUS_OK ||
            opus_decoder_ctl(decoder_, OPUS_SET_GAIN(gain)) != OPUS_OK) {
            return absl::InternalError("Failed to initialize Opus decoder");
        }
        header_state_ = HeaderState::kTags;
        return absl::OkStatus();
    }

    absl::Status ProcessAudioPackets(std::span<const PacketRef> packets,
                                     const OggPageView& page) {
        if (packets.empty()) {
            if (page.eos && header_state_ == HeaderState::kAudio) {
                return absl::DataLossError("Opus EOS page has no audio packet");
            }
            return absl::OkStatus();
        }
        if (header_state_ != HeaderState::kAudio) {
            return absl::DataLossError("Missing Opus headers");
        }

        std::array<std::size_t, kOggMaxSegments> packet_frames{};
        std::uint64_t page_frames = 0;
        for (std::size_t index = 0; index < packets.size(); ++index) {
            if (packets[index].bytes.size() >
                static_cast<std::size_t>(
                    (std::numeric_limits<opus_int32>::max)())) {
                return absl::ResourceExhaustedError("Opus packet is too large");
            }
            const auto frames = opus_packet_get_nb_samples(
                packets[index].bytes.data(),
                static_cast<opus_int32>(packets[index].bytes.size()),
                kSampleRate);
            if (frames <= 0 || frames > static_cast<int>(kMaxPacketFrames) ||
                page_frames > (std::numeric_limits<std::uint64_t>::max)() -
                                  static_cast<std::uint64_t>(frames)) {
                return absl::DataLossError("Invalid Opus audio packet");
            }
            packet_frames[index] = static_cast<std::size_t>(frames);
            page_frames += static_cast<std::uint64_t>(frames);
        }
        if (decoded_frames_ >
            (std::numeric_limits<std::uint64_t>::max)() - page_frames) {
            return absl::DataLossError("Opus timeline overflow");
        }
        const auto decoded_after = decoded_frames_ + page_frames;
        if (page.granule_position == kOggUnknownGranule) {
            return absl::DataLossError("Audio page lacks a granule position");
        }

        if (!has_origin_) {
            if (page.eos && page.granule_position < decoded_after) {
                origin_ = 0;
            } else {
                if (page.granule_position < decoded_after) {
                    return absl::DataLossError("Invalid initial Opus granule");
                }
                origin_ = page.granule_position - decoded_after;
            }
            has_origin_ = true;
        }
        if (page.granule_position < origin_ ||
            (has_granule_ && page.granule_position < previous_granule_)) {
            return absl::DataLossError("Regressing Opus granule");
        }
        const auto page_end = page.granule_position - origin_;
        if ((!page.eos && page_end != decoded_after) ||
            (page.eos &&
             (page_end > decoded_after || page_end < decoded_frames_))) {
            return absl::DataLossError("Inconsistent Opus page granule");
        }
        previous_granule_ = page.granule_position;
        has_granule_ = true;

        const auto valid_start = static_cast<std::uint64_t>(pre_skip_);
        const auto valid_end = page.eos ? page_end : decoded_after;
        std::array<opus_int16, kMaxPacketFrames * 2> decoded{};
        for (std::size_t index = 0; index < packets.size(); ++index) {
            const auto packet_start = decoded_frames_;
            const auto packet_end = packet_start + packet_frames[index];
            const auto produced = opus_decode(
                decoder_, packets[index].bytes.data(),
                static_cast<opus_int32>(packets[index].bytes.size()),
                decoded.data(), static_cast<int>(packet_frames[index]), 0);
            if (produced != static_cast<int>(packet_frames[index])) {
                return absl::DataLossError("Opus packet decode failed");
            }

            const auto keep_start = std::max(packet_start, valid_start);
            const auto keep_end = std::min(packet_end, valid_end);
            if (keep_end > keep_start) {
                const auto offset =
                    static_cast<std::size_t>(keep_start - packet_start);
                const auto frames =
                    static_cast<std::size_t>(keep_end - keep_start);
                if (emitted_frames_ >
                        (std::numeric_limits<std::uint64_t>::max)() - frames ||
                    emitted_frames_ + frames > expected_frames_) {
                    return absl::DataLossError(
                        "Opus output exceeds descriptor frame count");
                }
                AppendPcm(decoded.data(), offset, frames);
                emitted_frames_ += frames;
            }
            decoded_frames_ = packet_end;
        }
        return absl::OkStatus();
    }

    void AppendPcm(const opus_int16* decoded, std::size_t offset,
                   std::size_t frames) {
        const auto old_size = pcm_buffer_.size();
        if (frames >
            (BufferManaged::kMaxSize - old_size) / sizeof(std::int16_t)) {
            throw std::bad_alloc();
        }
        pcm_buffer_.resize(old_size + frames * sizeof(std::int16_t));
        auto* output =
            reinterpret_cast<std::int16_t*>(pcm_buffer_.data() + old_size);
        if (channels_ == 1) {
            std::memcpy(output, decoded + offset,
                        frames * sizeof(std::int16_t));
            return;
        }
        const auto* input = decoded + offset * 2;
        for (std::size_t index = 0; index < frames; ++index) {
            output[index] = static_cast<std::int16_t>(
                std::nearbyint((static_cast<float>(input[index * 2]) +
                                static_cast<float>(input[index * 2 + 1])) /
                               2.0F));
        }
    }

    absl::StatusOr<OpusFeedResult> FailFeed(absl::Status status) {
        failed_ = true;
        pcm_buffer_.clear();
        pcm_position_ = 0;
        return status;
    }
};

YSM_PIMPL_DEFINITION(OpusAudioStream)

OpusAudioStream::OpusAudioStream(std::uint64_t expected_frames)
    : YSM_PIMPL_CONSTRUCT(expected_frames) {}

OpusAudioStream::OpusAudioStream(const OpusAudioCheckpoint& checkpoint)
    : YSM_PIMPL_CONSTRUCT(checkpoint.Pimpl()) {}

OpusAudioStream::~OpusAudioStream() = default;

absl::StatusOr<std::size_t> OpusAudioStream::CheckpointSize() const {
    return Pimpl().CheckpointSize();
}

absl::StatusOr<std::unique_ptr<OpusAudioCheckpoint>>
OpusAudioStream::CaptureCheckpoint() const {
    const auto size = CheckpointSize();
    if (!size.ok()) {
        return size.status();
    }
    auto checkpoint =
        std::unique_ptr<OpusAudioCheckpoint>(new OpusAudioCheckpoint());
    Pimpl().Capture(checkpoint->Pimpl());
    return checkpoint;
}

absl::StatusOr<OpusFeedResult> OpusAudioStream::Feed(BufferViewR input) {
    return Pimpl().Feed(input);
}

void OpusAudioStream::EndInput() noexcept {
    Pimpl().EndInput();
}

std::int32_t OpusAudioStream::Decode(BufferView destination) {
    return Pimpl().Decode(destination);
}
}  // namespace ysm::codec
