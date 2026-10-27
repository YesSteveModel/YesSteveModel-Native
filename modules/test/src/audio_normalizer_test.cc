#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <codec/audio.h>
#include <codec/base64.h>

#include "audio_test_fixture.h"

namespace ysm::codec {
namespace {
void AppendLe(std::vector<Byte>& output, std::uint64_t value, int size) {
    for (int index = 0; index < size; ++index) {
        output.push_back(static_cast<Byte>(value >> (index * 8)));
    }
}

std::uint32_t IndependentCrc(BufferViewR page) {
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

void AppendPage(std::vector<Byte>& output, BufferViewR packet, Byte flags,
                std::uint64_t granule, std::uint32_t sequence,
                std::uint32_t serial) {
    ASSERT_LT(packet.size(), 255U);
    const auto begin = output.size();
    output.insert(output.end(), {'O', 'g', 'g', 'S', 0, flags});
    AppendLe(output, granule, 8);
    AppendLe(output, serial, 4);
    AppendLe(output, sequence, 4);
    AppendLe(output, 0, 4);
    output.push_back(1);
    output.push_back(static_cast<Byte>(packet.size()));
    output.insert(output.end(), packet.begin(), packet.end());
    const auto crc = IndependentCrc(BufferViewR(output).subspan(begin));
    for (int index = 0; index < 4; ++index) {
        output[begin + 22 + index] = static_cast<Byte>(crc >> (index * 8));
    }
}

std::vector<Byte> Tags() {
    constexpr std::string_view kVendor = "fixture";
    constexpr std::array<std::string_view, 3> kComments{
        "artist=discarded", "R128_TRACK_GAIN=-64", "r128_album_gain=32"};
    std::vector<Byte> result{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    AppendLe(result, kVendor.size(), 4);
    result.insert(result.end(), kVendor.begin(), kVendor.end());
    AppendLe(result, kComments.size(), 4);
    for (const auto comment : kComments) {
        AppendLe(result, comment.size(), 4);
        result.insert(result.end(), comment.begin(), comment.end());
    }
    return result;
}

std::vector<Byte> OpusFixture(std::uint64_t final_granule = 960) {
    constexpr std::uint32_t kSerial = 0x12345678U;
    constexpr std::array<Byte, 19> kHead{'O', 'p', 'u',  's',  'H', 'e',  'a',
                                         'd', 1,   1,    0,    0,   0x80, 0xbb,
                                         0,   0,   0xc0, 0xff, 0};
    constexpr std::array<Byte, 3> kAudio{0xf8, 0xff, 0xfe};
    const auto tags = Tags();
    std::vector<Byte> result;
    AppendPage(result, kHead, 0x02, 0, 0, kSerial);
    AppendPage(result, tags, 0, 0, 1, kSerial);
    AppendPage(result, kAudio, 0x04, final_granule, 2, kSerial);
    return result;
}

std::vector<Byte> OpusFixtureWithR128(std::string_view value) {
    constexpr std::uint32_t kSerial = 0x12345678U;
    constexpr std::array<Byte, 19> kHead{'O', 'p', 'u',  's',  'H', 'e',  'a',
                                         'd', 1,   1,    0,    0,   0x80, 0xbb,
                                         0,   0,   0xc0, 0xff, 0};
    constexpr std::array<Byte, 3> kAudio{0xf8, 0xff, 0xfe};
    constexpr std::string_view kVendor = "fixture";
    constexpr std::string_view kKey = "R128_TRACK_GAIN=";
    std::vector<Byte> tags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    AppendLe(tags, kVendor.size(), 4);
    tags.insert(tags.end(), kVendor.begin(), kVendor.end());
    AppendLe(tags, 1, 4);
    AppendLe(tags, kKey.size() + value.size(), 4);
    tags.insert(tags.end(), kKey.begin(), kKey.end());
    tags.insert(tags.end(), value.begin(), value.end());

    std::vector<Byte> result;
    AppendPage(result, kHead, 0x02, 0, 0, kSerial);
    AppendPage(result, tags, 0, 0, 1, kSerial);
    AppendPage(result, kAudio, 0x04, 960, 2, kSerial);
    return result;
}

BufferManaged Flatten(const NormalizedAudio& audio) {
    std::size_t size = audio.first_page.size();
    for (const auto& chunk : audio.chunks) {
        size += chunk.size();
    }
    BufferManaged result(size);
    std::size_t offset = 0;
    const auto append = [&](BufferViewR bytes) {
        std::memcpy(result.data() + offset, bytes.data(), bytes.size());
        offset += bytes.size();
    };
    append(audio.first_page);
    for (const auto& chunk : audio.chunks) {
        append(chunk);
    }
    return result;
}

bool Contains(BufferViewR bytes, std::string_view text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) !=
           bytes.end();
}

BufferManaged DecodeFixture(std::string_view encoded) {
    BufferManaged decoded(encoded.size() * 3 / 4 + 3);
    auto size = Base64Decode(StrBuf(encoded), decoded);
    EXPECT_TRUE(size.ok());
    if (!size.ok()) {
        return BufferManaged();
    }
    decoded.resize(*size);
    return decoded;
}

TEST(AudioNormalizerTest, RetainedOutputIsCanonicalAndIdempotent) {
    const auto source = OpusFixture();
    const auto options = NormalizeAudioOptions{
        .role = NormalizationRole::kRetained,
    };
    auto first = NormalizeAudio(source, options, [] { return false; });
    ASSERT_TRUE(first.ok()) << first.failure().status;
    EXPECT_EQ(first.value().facts.frames, 960U);
    EXPECT_EQ(first.value().facts.output_gain, -64);
    EXPECT_TRUE(ValidatePrefix(first.value()).ok());
    EXPECT_TRUE(ValidateComplete(first.value()).ok());

    auto canonical = Flatten(first.value());
    EXPECT_FALSE(Contains(canonical, "artist=discarded"));
    EXPECT_TRUE(Contains(canonical, "R128_TRACK_GAIN=-64"));
    EXPECT_TRUE(Contains(canonical, "R128_ALBUM_GAIN=32"));

    auto second = NormalizeAudio(canonical, options, [] { return false; });
    ASSERT_TRUE(second.ok()) << second.failure().status;
    auto canonical_again = Flatten(second.value());
    EXPECT_TRUE(Cmp(BufferViewR(canonical), BufferViewR(canonical_again)));
}

TEST(AudioNormalizerTest, AcceptsPositiveR128GainWithExplicitSign) {
    auto result = NormalizeAudio(
        OpusFixtureWithR128("+32"),
        NormalizeAudioOptions{.role = NormalizationRole::kRetained},
        [] { return false; });
    ASSERT_TRUE(result.ok()) << result.failure().status;
    EXPECT_TRUE(Contains(Flatten(result.value()), "R128_TRACK_GAIN=32"));
}

TEST(AudioNormalizerTest, RejectsR128GainWithMultipleSigns) {
    auto result = NormalizeAudio(
        OpusFixtureWithR128("+-1"),
        NormalizeAudioOptions{.role = NormalizationRole::kRetained},
        [] { return false; });
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.failure().kind, AudioFailureKind::kSourceInvalid);
    EXPECT_EQ(result.failure().stage, AudioStage::kInspect);
}

TEST(AudioNormalizerTest, TranscodedOutputUsesExactTrimAndCanonicalProfile) {
    const auto source = OpusFixture(480);
    const auto options = NormalizeAudioOptions{
        .role = NormalizationRole::kTranscoded,
        .transcoded_serial = 77,
    };
    auto result = NormalizeAudio(source, options, [] { return false; });
    ASSERT_TRUE(result.ok()) << result.failure().status;
    EXPECT_EQ(result.value().facts.codec, AudioCodec::kOpus);
    EXPECT_EQ(result.value().facts.channels, 1);
    EXPECT_EQ(result.value().facts.sample_rate, 48'000U);
    EXPECT_EQ(result.value().facts.frames, 480U);
    EXPECT_EQ(result.value().facts.serial_number, 77U);
    EXPECT_EQ(result.value().facts.output_gain, -64);
    EXPECT_GT(result.value().facts.pre_skip, 0U);
    EXPECT_TRUE(ValidatePrefix(result.value()).ok());
    EXPECT_TRUE(ValidateComplete(result.value()).ok());

    auto normalized = Flatten(result.value());
    EXPECT_FALSE(Contains(normalized, "R128_TRACK_GAIN"));
    EXPECT_FALSE(Contains(normalized, "R128_ALBUM_GAIN"));
}

TEST(AudioNormalizerTest, TranscodesIndependentVorbisRatesToExactTimeline) {
    struct Fixture {
        std::string_view encoded;
        std::uint32_t source_rate;
        std::uint64_t source_frames;
        std::uint64_t target_frames;
    };

    const std::array fixtures{
        Fixture{::ysm::test::kVorbis44100Base64, 44'100, 1'103, 1'201},
        Fixture{::ysm::test::kVorbis96000Base64, 96'000, 2'400, 1'200},
    };
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.source_rate);
        const auto source = DecodeFixture(fixture.encoded);
        ASSERT_FALSE(source.empty());
        auto source_facts = InspectAudio(source);
        ASSERT_TRUE(source_facts.ok()) << source_facts.status();
        EXPECT_EQ(source_facts->codec, AudioCodec::kVorbis);
        EXPECT_EQ(source_facts->sample_rate, fixture.source_rate);
        EXPECT_EQ(source_facts->frames, fixture.source_frames);

        auto normalized = NormalizeAudio(
            source,
            NormalizeAudioOptions{.role = NormalizationRole::kTranscoded},
            [] { return false; });
        ASSERT_TRUE(normalized.ok()) << normalized.failure().status;
        EXPECT_EQ(normalized.value().facts.sample_rate, 48'000U);
        EXPECT_EQ(normalized.value().facts.channels, 1);
        EXPECT_EQ(normalized.value().facts.frames, fixture.target_frames);
        EXPECT_TRUE(ValidatePrefix(normalized.value()).ok());
        EXPECT_TRUE(ValidateComplete(normalized.value()).ok());
    }
}

TEST(AudioNormalizerTest, HandlesDeepTrimEmptyInputAndMidDecodeCancellation) {
    auto short_result = NormalizeAudio(
        OpusFixture(1),
        NormalizeAudioOptions{.role = NormalizationRole::kTranscoded},
        [] { return false; });
    ASSERT_TRUE(short_result.ok()) << short_result.failure().status;
    EXPECT_EQ(short_result.value().facts.frames, 1U);
    EXPECT_TRUE(ValidateComplete(short_result.value()).ok());

    auto empty = NormalizeAudio(
        {}, NormalizeAudioOptions{.role = NormalizationRole::kTranscoded},
        [] { return false; });
    ASSERT_FALSE(empty.ok());
    EXPECT_EQ(empty.failure().kind, AudioFailureKind::kSourceInvalid);

    const auto vorbis = DecodeFixture(::ysm::test::kVorbis44100Base64);
    int polls = 0;
    auto cancelled = NormalizeAudio(
        vorbis, NormalizeAudioOptions{.role = NormalizationRole::kTranscoded},
        [&polls] { return ++polls >= 3; });
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.failure().kind, AudioFailureKind::kCancelled);
    EXPECT_EQ(cancelled.failure().stage, AudioStage::kDecode);
}

TEST(AudioNormalizerTest, CancellationAndCorruptionDoNotPublishCandidate) {
    auto source = OpusFixture();
    auto cancelled = NormalizeAudio(
        source, NormalizeAudioOptions{.role = NormalizationRole::kRetained},
        [] { return true; });
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.failure().kind, AudioFailureKind::kCancelled);

    source.back() ^= 1U;
    auto corrupt = NormalizeAudio(
        source, NormalizeAudioOptions{.role = NormalizationRole::kRetained},
        [] { return false; });
    ASSERT_FALSE(corrupt.ok());
    EXPECT_EQ(corrupt.failure().kind, AudioFailureKind::kSourceInvalid);
    EXPECT_EQ(corrupt.failure().stage, AudioStage::kInspect);
}
}  // namespace
}  // namespace ysm::codec
