#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <opus/opus.h>

#include <codec/opus.h>

namespace ysm::codec {
namespace {
std::uint32_t OggCrc(std::span<const Byte> bytes) {
    std::uint32_t crc = 0;
    for (const auto value : bytes) {
        crc ^= static_cast<std::uint32_t>(value) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc << 1U) ^ ((crc & 0x80000000U) != 0 ? 0x04C11DB7U : 0U);
        }
    }
    return crc;
}

void AppendLe(std::vector<Byte>& output, std::uint64_t value, int size) {
    for (int index = 0; index < size; ++index) {
        output.push_back(static_cast<Byte>(value >> (index * 8)));
    }
}

void AppendLacedPage(std::vector<Byte>& output, std::span<const Byte> lacing,
                     std::span<const Byte> body, Byte flags,
                     std::uint64_t granule, std::uint32_t sequence) {
    const auto start = output.size();
    output.insert(output.end(), {'O', 'g', 'g', 'S', 0, flags});
    AppendLe(output, granule, 8);
    AppendLe(output, 7, 4);
    AppendLe(output, sequence, 4);
    AppendLe(output, 0, 4);
    output.push_back(static_cast<Byte>(lacing.size()));
    output.insert(output.end(), lacing.begin(), lacing.end());
    output.insert(output.end(), body.begin(), body.end());
    const auto crc = OggCrc(std::span(output).subspan(start));
    for (int index = 0; index < 4; ++index) {
        output[start + 22 + index] = static_cast<Byte>(crc >> (index * 8));
    }
}

void AppendPage(std::vector<Byte>& output, std::span<const Byte> packet,
                Byte flags, std::uint64_t granule, std::uint32_t sequence) {
    const std::array<Byte, 1> lacing{static_cast<Byte>(packet.size())};
    AppendLacedPage(output, lacing, packet, flags, granule, sequence);
}

std::pair<std::vector<Byte>, std::size_t> MinimalOpus() {
    constexpr std::array<Byte, 19> kHead{'O', 'p', 'u', 's', 'H', 'e',  'a',
                                         'd', 1,   1,   0,   0,   0x80, 0xbb,
                                         0,   0,   0,   0,   0};
    constexpr std::array<Byte, 16> kTags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's',
                                         0,   0,   0,   0,   0,   0,   0,   0};
    constexpr std::array<Byte, 3> kAudio{0xf8, 0xff, 0xfe};

    std::vector<Byte> encoded;
    AppendPage(encoded, kHead, 0x02, 0, 0);
    AppendPage(encoded, kTags, 0, 0, 1);
    const auto audio_page_offset = encoded.size();
    AppendPage(encoded, kAudio, 0x04, 960, 2);
    return {std::move(encoded), audio_page_offset};
}

TEST(OpusAudioStreamTest, DecodeCanPropagateAllocationFailure) {
    EXPECT_FALSE(noexcept(
        std::declval<OpusAudioStream&>().Decode(std::declval<BufferView>())));
}

TEST(OpusAudioStreamTest, RequestsMoreInputWhenPageHeaderIsSplitAcrossChunks) {
    auto [encoded, audio_page_offset] = MinimalOpus();
    constexpr std::size_t kHeaderFragmentSize = 19;
    const auto split = audio_page_offset + kHeaderFragmentSize;

    OpusAudioStream decoder(960);
    const auto first = decoder.Feed(BufferViewR(encoded).first(split));
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first->consumed, split);
    std::array<Byte, 960 * sizeof(std::int16_t)> pcm{};
    EXPECT_EQ(decoder.Decode(pcm), OpusAudioStream::kNeedInput);

    const auto second = decoder.Feed(BufferViewR(encoded).subspan(split));
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second->consumed, encoded.size() - split);
    decoder.EndInput();
    EXPECT_EQ(decoder.Decode(pcm), pcm.size());
    EXPECT_EQ(decoder.Decode(pcm), 0);
}

void AppendPacket(std::vector<Byte>& lacing, std::vector<Byte>& body,
                  std::span<const Byte> packet) {
    auto remaining = packet.size();
    while (remaining >= 255) {
        lacing.push_back(255);
        remaining -= 255;
    }
    lacing.push_back(static_cast<Byte>(remaining));
    body.insert(body.end(), packet.begin(), packet.end());
}

struct CheckpointFixture {
    std::vector<Byte> prefix;
    std::vector<Byte> tail;
    std::uint64_t expected_frames{};
};

CheckpointFixture EncodedFixture(int channels, std::uint16_t pre_skip,
                                 std::int16_t gain, std::uint64_t origin,
                                 bool partial_packet) {
    constexpr int kPacketFrames = 960;
    constexpr int kPacketCount = 8;
    constexpr int kPrefixPackets = 3;
    constexpr int kEndTrim = 117;
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
        opus_encoder_create(48'000, channels, OPUS_APPLICATION_AUDIO, &error),
        opus_encoder_destroy);
    EXPECT_EQ(error, OPUS_OK);
    EXPECT_NE(encoder, nullptr);
    if (!encoder) {
        return {};
    }
    EXPECT_EQ(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(256'000)),
              OPUS_OK);
    EXPECT_EQ(opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)), OPUS_OK);
    std::array<std::vector<Byte>, kPacketCount> packets;
    for (int packet_index = 0; packet_index < kPacketCount; ++packet_index) {
        std::vector<opus_int16> samples(kPacketFrames * channels);
        for (int frame = 0; frame < kPacketFrames; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                samples[frame * channels + channel] =
                    static_cast<opus_int16>(
                        13'000 * std::sin((packet_index * kPacketFrames + frame) *
                                         (0.035 + channel * 0.009)));
            }
        }
        std::array<Byte, 4'000> encoded{};
        const auto size = opus_encode(encoder.get(), samples.data(),
                                      kPacketFrames, encoded.data(),
                                      static_cast<opus_int32>(encoded.size()));
        EXPECT_GT(size, 255);
        if (size <= 255) {
            return {};
        }
        packets[packet_index].assign(encoded.begin(), encoded.begin() + size);
    }

    std::array<Byte, 19> head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1};
    head[9] = static_cast<Byte>(channels);
    head[10] = static_cast<Byte>(pre_skip);
    head[11] = static_cast<Byte>(pre_skip >> 8);
    head[12] = 0x80;
    head[13] = 0xbb;
    head[16] = static_cast<Byte>(gain);
    head[17] = static_cast<Byte>(static_cast<std::uint16_t>(gain) >> 8);
    constexpr std::array<Byte, 16> kTags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    CheckpointFixture fixture;
    fixture.expected_frames =
        kPacketCount * kPacketFrames - pre_skip - kEndTrim;
    AppendPage(fixture.prefix, head, 0x02, 0, 0);
    AppendPage(fixture.prefix, kTags, 0, 0, 1);

    std::vector<Byte> lacing;
    std::vector<Byte> body;
    for (int index = 0; index < kPrefixPackets; ++index) {
        AppendPacket(lacing, body, packets[index]);
    }
    if (partial_packet) {
        lacing.push_back(255);
        body.insert(body.end(), packets[kPrefixPackets].begin(),
                    packets[kPrefixPackets].begin() + 255);
    }
    AppendLacedPage(fixture.prefix, lacing, body, 0,
                    origin + kPrefixPackets * kPacketFrames, 2);
    lacing.clear();
    body.clear();
    for (int index = kPrefixPackets; index < kPacketCount; ++index) {
        const auto packet = std::span(packets[index]);
        AppendPacket(lacing, body,
                      partial_packet && index == kPrefixPackets
                          ? packet.subspan(255)
                          : packet);
    }
    AppendLacedPage(fixture.tail, lacing, body,
                    partial_packet ? 0x05 : 0x04,
                    origin + kPacketCount * kPacketFrames - kEndTrim, 3);
    return fixture;
}

std::int32_t Drain(OpusAudioStream& decoder, std::vector<Byte>& output,
                    std::size_t pcm_bytes = 514) {
    std::vector<Byte> pcm(pcm_bytes);
    for (;;) {
        const auto size = decoder.Decode(pcm);
        if (size <= 0) {
            return size;
        }
        output.insert(output.end(), pcm.begin(), pcm.begin() + size);
    }
}

void FeedAndDrain(OpusAudioStream& decoder, BufferViewR input,
                   std::vector<Byte>& output, std::size_t pcm_bytes = 514) {
    while (!input.empty()) {
        const auto fed = decoder.Feed(input);
        ASSERT_TRUE(fed.ok()) << fed.status();
        ASSERT_GT(fed->consumed, 0);
        input = input.subspan(fed->consumed);
        EXPECT_EQ(Drain(decoder, output, pcm_bytes), OpusAudioStream::kNeedInput);
    }
}

std::vector<Byte> DecodeFixture(const CheckpointFixture& fixture) {
    OpusAudioStream decoder(fixture.expected_frames);
    std::vector<Byte> pcm;
    FeedAndDrain(decoder, fixture.prefix, pcm);
    FeedAndDrain(decoder, fixture.tail, pcm);
    decoder.EndInput();
    EXPECT_EQ(Drain(decoder, pcm), 0);
    EXPECT_EQ(pcm.size(), fixture.expected_frames * 2);
    return pcm;
}

class OpusCheckpointTest : public ::testing::TestWithParam<int> {};

TEST_P(OpusCheckpointTest, RestoresCodecGainTimelineAndPartialPacket) {
    for (const bool partial_packet : {false, true}) {
        SCOPED_TRACE(partial_packet);
        const auto fixture =
            EncodedFixture(GetParam(), 312, 1'024, 123'456, partial_packet);
        const auto baseline = DecodeFixture(fixture);
        OpusAudioStream original(fixture.expected_frames);
        std::vector<Byte> prefix_pcm;
        FeedAndDrain(original, fixture.prefix, prefix_pcm);
        const auto measured = original.CheckpointSize();
        ASSERT_TRUE(measured.ok()) << measured.status();
        EXPECT_EQ(*measured,
                  sizeof(OpusAudioCheckpoint) +
                      opus_decoder_get_size(GetParam()) +
                      (partial_packet ? 255 : 0));
        auto captured = original.CaptureCheckpoint();
        ASSERT_TRUE(captured.ok()) << captured.status();
        auto checkpoint = std::move(*captured);
        EXPECT_EQ(checkpoint->OwnedBytes(), *measured);
        EXPECT_EQ(checkpoint->EmittedFrames(), prefix_pcm.size() / 2);

        OpusAudioStream restored(*checkpoint);
        auto resumed_pcm = prefix_pcm;
        FeedAndDrain(restored, fixture.tail, resumed_pcm, 2'048);
        restored.EndInput();
        EXPECT_EQ(Drain(restored, resumed_pcm), 0);
        EXPECT_EQ(resumed_pcm, baseline);
        auto original_pcm = prefix_pcm;
        FeedAndDrain(original, fixture.tail, original_pcm);
        original.EndInput();
        EXPECT_EQ(Drain(original, original_pcm), 0);
        EXPECT_EQ(original_pcm, baseline);
    }
}

TEST_P(OpusCheckpointTest, SharedCheckpointSupportsIndependentConcurrentRestore) {
    const auto fixture = EncodedFixture(GetParam(), 1'200, -768, 99'000, true);
    const auto baseline = DecodeFixture(fixture);
    auto original = std::make_unique<OpusAudioStream>(fixture.expected_frames);
    std::vector<Byte> prefix_pcm;
    FeedAndDrain(*original, fixture.prefix, prefix_pcm);
    auto captured = original->CaptureCheckpoint();
    ASSERT_TRUE(captured.ok()) << captured.status();
    const auto checkpoint = std::move(*captured);
    original.reset();
    const auto resume = [&](std::size_t fragment_size) {
        OpusAudioStream restored(*checkpoint);
        auto pcm = prefix_pcm;
        for (std::size_t offset = 0; offset < fixture.tail.size();) {
            const auto size =
                std::min(fragment_size, fixture.tail.size() - offset);
            FeedAndDrain(restored,
                          BufferViewR(fixture.tail).subspan(offset, size), pcm);
            offset += size;
        }
        restored.EndInput();
        EXPECT_EQ(Drain(restored, pcm), 0);
        return pcm;
    };
    auto first = std::async(std::launch::async, resume, 17);
    auto second = std::async(std::launch::async, resume, 431);
    EXPECT_EQ(first.get(), baseline);
    EXPECT_EQ(second.get(), baseline);
}

TEST_P(OpusCheckpointTest, DestroyingAnInterleavedRestoreLeavesOtherRestoresValid) {
    const auto fixture = EncodedFixture(GetParam(), 312, 512, 75'000, true);
    const auto baseline = DecodeFixture(fixture);
    OpusAudioStream original(fixture.expected_frames);
    std::vector<Byte> prefix_pcm;
    FeedAndDrain(original, fixture.prefix, prefix_pcm);
    auto captured = original.CaptureCheckpoint();
    ASSERT_TRUE(captured.ok()) << captured.status();
    const auto checkpoint = std::move(*captured);
    auto first = std::make_unique<OpusAudioStream>(*checkpoint);
    OpusAudioStream second(*checkpoint);
    std::vector<Byte> abandoned_pcm;
    auto resumed_pcm = prefix_pcm;
    const auto split = fixture.tail.size() / 2;
    const auto fragment = BufferViewR(fixture.tail).first(split);
    FeedAndDrain(*first, fragment, abandoned_pcm);
    FeedAndDrain(second, fragment, resumed_pcm);
    first.reset();
    FeedAndDrain(second, BufferViewR(fixture.tail).subspan(split), resumed_pcm);
    second.EndInput();
    EXPECT_EQ(Drain(second, resumed_pcm), 0);
    EXPECT_EQ(resumed_pcm, baseline);
}

TEST_P(OpusCheckpointTest, RestoresBeforePreskipHasFinished) {
    const auto fixture = EncodedFixture(GetParam(), 4'000, 768, 123'000, true);
    const auto baseline = DecodeFixture(fixture);
    OpusAudioStream original(fixture.expected_frames);
    std::vector<Byte> pcm;
    FeedAndDrain(original, fixture.prefix, pcm);
    ASSERT_TRUE(pcm.empty());
    auto captured = original.CaptureCheckpoint();
    ASSERT_TRUE(captured.ok()) << captured.status();
    EXPECT_EQ((*captured)->EmittedFrames(), 0);
    OpusAudioStream restored(**captured);
    FeedAndDrain(restored, fixture.tail, pcm);
    restored.EndInput();
    EXPECT_EQ(Drain(restored, pcm), 0);
    EXPECT_EQ(pcm, baseline);
}

INSTANTIATE_TEST_SUITE_P(SourceChannels, OpusCheckpointTest,
                         ::testing::Values(1, 2));

TEST(OpusAudioStreamTest, CheckpointRejectsPendingPcmIncompletePageAndTerminalStates) {
    const auto fixture = EncodedFixture(1, 312, 0, 0, false);
    OpusAudioStream decoder(fixture.expected_frames);
    EXPECT_FALSE(decoder.CheckpointSize().ok());
    EXPECT_FALSE(decoder.CaptureCheckpoint().ok());
    const auto fed = decoder.Feed(fixture.prefix);
    ASSERT_TRUE(fed.ok());
    ASSERT_EQ(fed->consumed, fixture.prefix.size());
    EXPECT_FALSE(decoder.CheckpointSize().ok());
    EXPECT_FALSE(decoder.CaptureCheckpoint().ok());
    std::vector<Byte> pcm;
    EXPECT_EQ(Drain(decoder, pcm), OpusAudioStream::kNeedInput);
    ASSERT_TRUE(decoder.CheckpointSize().ok());
    const auto fragment = decoder.Feed(BufferViewR(fixture.tail).first(19));
    ASSERT_TRUE(fragment.ok());
    EXPECT_FALSE(decoder.CheckpointSize().ok());
    EXPECT_FALSE(decoder.CaptureCheckpoint().ok());
    FeedAndDrain(decoder, BufferViewR(fixture.tail).subspan(19), pcm);
    EXPECT_FALSE(decoder.CaptureCheckpoint().ok());
    decoder.EndInput();
    EXPECT_EQ(Drain(decoder, pcm), 0);
    EXPECT_FALSE(decoder.CaptureCheckpoint().ok());

    OpusAudioStream ended(fixture.expected_frames);
    FeedAndDrain(ended, fixture.prefix, pcm);
    ended.EndInput();
    EXPECT_FALSE(ended.CaptureCheckpoint().ok());

    OpusAudioStream failed(fixture.expected_frames);
    FeedAndDrain(failed, fixture.prefix, pcm);
    std::array<Byte, 1> invalid_output{};
    EXPECT_EQ(failed.Decode(invalid_output), OpusAudioStream::kError);
    EXPECT_FALSE(failed.CheckpointSize().ok());
    EXPECT_FALSE(failed.CaptureCheckpoint().ok());
}
}  // namespace
}  // namespace ysm::codec
