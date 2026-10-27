#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <v3/container/decoder.h>
#include <v3/container/image.h>
#include <v3/conversion/projector.h>

#include <codec/base64.h>

#include "audio_test_fixture.h"
#include "proto/asset/model/ModelData.proto.h"
#include "proto/asset/model/data/animation.proto.h"
#include "proto/asset/model/data/animation_controller.proto.h"
#include "proto/asset/model/data/geo_model.proto.h"
#include "proto/asset/strings/StringData.proto.h"
#include "proto/manifest/manifest.proto.h"

namespace ysm::legacy::v3::container {
namespace {
namespace proto = ysm::proto;

constexpr std::string_view kModelHash = "000102030405060708090a0b0c0d0e0f";
constexpr std::size_t kMaxPayloadCount = 32'766;

struct SoundFixture {
    std::string_view name;
    std::span<const Byte> bytes;
};

class FixtureWriter final {
   public:
    void FixedU32(std::uint32_t value) {
        for (int index = 0; index < 4; ++index) {
            bytes_.push_back(static_cast<Byte>(value >> (index * 8)));
        }
    }

    void VarU32(std::uint32_t value) {
        do {
            auto byte = static_cast<Byte>(value & 0x7FU);
            value >>= 7U;
            bytes_.push_back(value == 0 ? byte
                                        : static_cast<Byte>(byte | 0x80U));
        } while (value != 0);
    }

    void VarU64(std::uint64_t value) {
        do {
            auto byte = static_cast<Byte>(value & 0x7FU);
            value >>= 7U;
            bytes_.push_back(value == 0 ? byte
                                        : static_cast<Byte>(byte | 0x80U));
        } while (value != 0);
    }

    void Bool(bool value) { bytes_.push_back(value ? 1 : 0); }

    void Float(float value) { FixedU32(std::bit_cast<std::uint32_t>(value)); }

    void HalfBits(std::uint16_t bits) {
        bytes_.push_back(static_cast<Byte>(bits));
        bytes_.push_back(static_cast<Byte>(bits >> 8U));
    }

    void String(std::string_view value) {
        VarU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void Bytes(std::span<const Byte> value) {
        VarU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    [[nodiscard]] std::vector<Byte> Finish() && { return std::move(bytes_); }

   private:
    std::vector<Byte> bytes_;
};

void WriteEmptyStringMap(FixtureWriter& writer) {
    writer.VarU32(0);
}

void WriteGeoProperties(FixtureWriter& writer, bool half_float = false,
                        bool nonfinite = false) {
    writer.String("geometry.test");
    if (half_float) {
        writer.HalfBits(0x3D00);
    } else {
        writer.Float(nonfinite ? std::numeric_limits<float>::quiet_NaN()
                               : 1.25F);
    }
    writer.Float(64.0F);
    writer.Float(2.0F);
    writer.Float(2.0F);
    writer.VarU32(3);
    writer.Float(0.0F);
    writer.Float(1.0F);
    writer.Float(0.0F);
    writer.Float(0.7F);
    writer.Float(0.7F);
    writer.VarU32(0);  // extra info
    WriteEmptyStringMap(writer);
    writer.VarU32(0);  // initialize
    writer.VarU32(0);  // pre-animation
}

void WriteQuad(FixtureWriter& writer) {
    writer.Float(0.0F);
    writer.Float(0.0F);
    writer.Float(1.0F);
    constexpr std::array<std::array<float, 5>, 4> kVertices{{
        {0, 0, 0, 0, 0},
        {1, 0, 0, 1, 0},
        {1, 1, 0, 1, 1},
        {0, 1, 0, 0, 1},
    }};
    for (const auto& vertex : kVertices) {
        for (const auto component : vertex) {
            writer.Float(component);
        }
    }
}

void WriteGeo(FixtureWriter& writer, std::uint32_t version,
              std::string_view name, bool v5_face = false,
              bool half_float = false, std::string_view parent = {},
              bool nonfinite = false) {
    writer.VarU32(1);  // bones
    writer.String(parent);
    if (version == 5) {
        writer.VarU32(v5_face ? 1 : 0);
        if (v5_face) {
            WriteQuad(writer);
        }
    } else {
        writer.VarU32(0);  // cubes
    }
    writer.String(name);
    for (int index = 0; index < 5; ++index) {
        writer.Bool(false);
    }
    for (int index = 0; index < 6; ++index) {
        writer.Float(0.0F);
    }
    WriteGeoProperties(writer, half_float, nonfinite);
    if (version == 5) {
        writer.VarU32(v5_face ? 1 : 0);
    }
}

void WriteModelProperties(FixtureWriter& writer, std::uint32_t version) {
    writer.Float(0.7F);
    writer.Float(0.7F);
    WriteEmptyStringMap(writer);
    if (version >= 12) {
        writer.VarU32(0);  // buttons
        writer.VarU32(0);  // classifications
    }
    writer.String("skin");
    writer.String("");
    writer.Bool(false);  // free
    if (version >= 8) {
        writer.Bool(false);
    }
    if (version >= 13) {
        writer.Bool(false);
    }
    if (version >= 14) {
        writer.Bool(false);
    }
    if (version >= 17) {
        writer.Bool(false);
    }
    if (version >= 32) {
        writer.Bool(false);
    }
    if (version >= 20) {
        writer.String("");
        writer.String("");
    }
}

void WriteRgbaImage(FixtureWriter& writer, std::uint32_t version,
                    bool legacy_png) {
    constexpr std::array<Byte, 4> kPixel{0x10, 0x20, 0x30, 0xFF};
    writer.Bytes(kPixel);
    writer.VarU32(1);
    writer.VarU32(1);
    if (!legacy_png && version >= 23) {
        writer.VarU32(1);  // historical RGBA enum
        writer.VarU32(1);
    }
}

void WriteCurrentTexture(FixtureWriter& writer, std::uint32_t version) {
    writer.String("");  // source hash
    WriteRgbaImage(writer, version, false);
    writer.VarU32(0);  // PBR
}

void WritePlayer(FixtureWriter& writer, std::uint32_t version,
                 bool half_float = false, std::string_view parent = {},
                 bool nonfinite = false,
                 bool infinite_animation_length = false) {
    writer.VarU32(infinite_animation_length ? 1 : 0);  // animations
    if (infinite_animation_length) {
        writer.VarU32(1);   // main animation type
        writer.String("");  // source hash
        writer.VarU32(1);   // animations in file
        writer.String("idle");
        writer.Float(std::numeric_limits<float>::infinity());
        writer.VarU32(1);  // loop
        for (int index = 0; index < 4; ++index) {
            writer.VarU32(0);  // animation optionals
        }
        writer.VarU32(0);  // bones
        writer.VarU32(0);  // instructions
        writer.VarU32(0);  // sounds
    }
    writer.VarU32(0);  // controllers
    writer.VarU32(1);  // textures
    writer.String("skin");
    WriteCurrentTexture(writer, version);
    writer.VarU32(2);  // geos
    for (const auto [type, name] : {std::pair{1U, std::string_view("main")},
                                    std::pair{2U, std::string_view("arm")}}) {
        writer.VarU32(type);
        writer.String("");  // source hash
        WriteGeo(writer, version, name, false, half_float && type == 1,
                 type == 1 ? parent : std::string_view{},
                 nonfinite && type == 1);
    }
}

void WriteCurrent(FixtureWriter& writer, std::uint32_t version,
                  std::string_view hash, bool half_float,
                  std::string_view parent, bool nonfinite,
                  bool infinite_animation_length,
                  std::span<const SoundFixture> sounds = {}) {
    writer.VarU32(static_cast<std::uint32_t>(sounds.size()));
    for (const auto& sound : sounds) {
        writer.String(sound.name);
        writer.String("");  // source hash
        writer.Bytes(sound.bytes);
    }
    writer.VarU32(0);  // functions
    writer.VarU32(0);  // languages
    if (version >= 27) {
        writer.VarU32(0);  // vehicles
        writer.VarU32(0);  // projectiles
    } else {
        writer.VarU32(0);  // projectile map
        if (version >= 21) {
            writer.VarU32(0);  // vehicle map
        }
    }
    writer.VarU32(1);  // player
    WritePlayer(writer, version, half_float, parent, nonfinite,
                infinite_animation_length);
    writer.String(hash);
    writer.VarU32(0);  // metadata
    WriteModelProperties(writer, version);
    writer.VarU32(0);  // author avatars
    if (version >= 20) {
        writer.VarU32(0);  // GUI images
    }
    if (version >= 27) {
        writer.VarU32(0);  // origin version
    }
    writer.VarU32(0);  // export info
    writer.String("");
}

void WriteLegacyTexture(FixtureWriter& writer, std::uint32_t version) {
    if (version >= 3) {
        WriteRgbaImage(writer, version, true);
        writer.VarU32(0);  // PBR
    } else {
        writer.VarU32(1);  // optional legacy PNG
        WriteRgbaImage(writer, version, true);
    }
}

void WriteLegacyInfo(FixtureWriter& writer, std::uint32_t version) {
    writer.VarU32(0);  // metadata
    WriteModelProperties(writer, version);
}

void WriteLegacy(FixtureWriter& writer, std::uint32_t version,
                 std::string_view hash, bool v5_face, std::string_view parent,
                 bool nonfinite) {
    writer.VarU32(0);  // features
    writer.VarU32(2);  // geos
    for (const auto [type, name] : {std::pair{1U, std::string_view("main")},
                                    std::pair{2U, std::string_view("arm")}}) {
        writer.VarU32(type);
        writer.VarU32(1);  // optional geo
        WriteGeo(writer, version, name, v5_face && type == 1, false,
                 type == 1 ? parent : std::string_view{},
                 nonfinite && type == 1);
    }
    writer.VarU32(0);  // animations
    if (version >= 10) {
        writer.VarU32(0);  // controllers
        writer.VarU32(0);  // controller hashes
    }
    writer.VarU32(1);  // textures
    writer.String(version == 1 ? "skin.png" : "skin");
    WriteLegacyTexture(writer, version);
    if (version >= 11) {
        writer.VarU32(0);  // sounds
        writer.VarU32(0);  // sound hashes
    }
    if (version >= 16) {
        writer.VarU32(0);  // functions
        writer.VarU32(0);  // function hashes
    }
    if (version >= 18) {
        writer.VarU32(0);  // languages
        writer.VarU32(0);  // language hashes
    }
    if (version >= 4) {
        writer.VarU32(0);  // avatars
    }
    writer.VarU32(0);  // geo hashes
    writer.VarU32(0);  // animation hashes
    writer.VarU32(1);  // texture hashes
    writer.String(version == 1 ? "skin.png" : "skin");
    if (version >= 3) {
        writer.String("");
        writer.VarU32(0);  // PBR hashes
    } else {
        writer.String("");
    }
    writer.String(hash);
    if (version >= 2) {
        writer.VarU32(1);
        WriteLegacyInfo(writer, version);
    }
    if (version >= 6) {
        writer.VarU32(0);  // export info
    }
}

std::vector<Byte> MakeFixture(std::uint32_t version,
                              std::string_view hash = kModelHash,
                              bool v5_face = false, bool half_float = false,
                              std::string_view parent = {},
                              bool nonfinite = false,
                              bool infinite_animation_length = false) {
    FixtureWriter writer;
    writer.FixedU32(version);
    if (version < 19) {
        WriteLegacy(writer, version, hash, v5_face, parent, nonfinite);
    } else {
        WriteCurrent(writer, version, hash, half_float, parent, nonfinite,
                     infinite_animation_length);
    }
    return std::move(writer).Finish();
}

std::vector<Byte> MakeCurrentSoundFixture(std::span<const Byte> sound) {
    FixtureWriter writer;
    writer.FixedU32(32);
    const std::array sounds{SoundFixture{"sound", sound}};
    WriteCurrent(writer, 32, kModelHash, false, {}, false, false, sounds);
    return std::move(writer).Finish();
}

void ExpectFailure(std::span<const Byte> bytes, absl::StatusCode status) {
    auto result = Decode(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), status);
}

template <typename Message>
Message ParsePayload(const Payload& payload) {
    Message result{};
    result.from_pb(
        std::string(reinterpret_cast<const char*>(payload.bytes.data()),
                    payload.bytes.size()));
    return result;
}

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

void AppendOggPage(std::vector<Byte>& output, std::span<const Byte> packet,
                   Byte flags, std::uint64_t granule, std::uint32_t sequence,
                   std::uint32_t serial) {
    if (packet.size() >= 255) {
        return;
    }
    const auto begin = output.size();
    output.insert(output.end(), {'O', 'g', 'g', 'S', 0, flags});
    for (int index = 0; index < 8; ++index) {
        output.push_back(static_cast<Byte>(granule >> (index * 8)));
    }
    for (int index = 0; index < 4; ++index) {
        output.push_back(static_cast<Byte>(serial >> (index * 8)));
    }
    for (int index = 0; index < 4; ++index) {
        output.push_back(static_cast<Byte>(sequence >> (index * 8)));
    }
    output.insert(output.end(), 4, 0);
    output.push_back(1);
    output.push_back(static_cast<Byte>(packet.size()));
    output.insert(output.end(), packet.begin(), packet.end());
    const auto crc = OggCrc(std::span(output).subspan(begin));
    for (int index = 0; index < 4; ++index) {
        output[begin + 22 + index] = static_cast<Byte>(crc >> (index * 8));
    }
}

std::vector<Byte> MinimalOgg(std::span<const Byte> identification) {
    std::vector<Byte> result;
    AppendOggPage(result, identification, 0x06, 960, 0, 1);
    return result;
}

std::vector<Byte> OpusIdentification() {
    std::vector<Byte> packet(19);
    constexpr std::array<Byte, 8> kOpus{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    std::copy(kOpus.begin(), kOpus.end(), packet.begin());
    packet[8] = 1;
    packet[9] = 2;
    constexpr std::array<Byte, 16> kTags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's',
                                         0,   0,   0,   0,   0,   0,   0,   0};
    constexpr std::array<Byte, 3> kAudio{0xf8, 0xff, 0xfe};
    std::vector<Byte> result;
    AppendOggPage(result, packet, 0x02, 0, 0, 1);
    AppendOggPage(result, kTags, 0, 0, 1, 1);
    AppendOggPage(result, kAudio, 0x04, 960, 2, 1);
    return result;
}

std::vector<Byte> VorbisIdentification() {
    const auto encoded = ::ysm::test::kVorbis44100Base64;
    std::vector<Byte> decoded(encoded.size() * 3 / 4 + 3);
    auto size = ::ysm::codec::Base64Decode(StrBuf(encoded), decoded);
    EXPECT_TRUE(size.ok());
    if (!size.ok()) {
        return {};
    }
    decoded.resize(*size);
    return decoded;
}

TEST(LegacyHistoricalDecoderTest, ClassifiesHistoricalSoundsBeforeMapCommit) {
    const auto opus = OpusIdentification();
    auto playable = Decode(MakeCurrentSoundFixture(opus));
    ASSERT_TRUE(playable.ok()) << playable.status();
    EXPECT_EQ(playable->sound_field_count, 1);
    EXPECT_EQ(playable->omitted_sound_count, 0);
    ASSERT_EQ(playable->common.sounds.size(), 1);
    EXPECT_EQ(playable->common.sounds.front().second.value.encoding,
              SoundEncoding::kOpus);
    EXPECT_EQ(playable->common.sounds.front().second.value.channels, 2);
    EXPECT_EQ(playable->common.sounds.front().second.value.sample_rate, 48'000);
    EXPECT_EQ(playable->common.sounds.front().second.value.samples, 960);

    const auto vorbis = Decode(MakeCurrentSoundFixture(VorbisIdentification()));
    ASSERT_TRUE(vorbis.ok()) << vorbis.status();
    ASSERT_EQ(vorbis->common.sounds.size(), 1);
    EXPECT_EQ(vorbis->common.sounds.front().second.value.encoding,
              SoundEncoding::kVorbis);
    EXPECT_EQ(vorbis->common.sounds.front().second.value.channels, 2);
    EXPECT_EQ(vorbis->common.sounds.front().second.value.sample_rate, 44'100);
    EXPECT_EQ(vorbis->common.sounds.front().second.value.samples, 1'103);

    constexpr std::array<Byte, 6> kMp3{'I', 'D', '3', 4, 0, 0};
    auto unknown = Decode(MakeCurrentSoundFixture(kMp3));
    ASSERT_TRUE(unknown.ok()) << unknown.status();
    EXPECT_EQ(unknown->sound_field_count, 1);
    EXPECT_EQ(unknown->omitted_sound_count, 1);
    EXPECT_TRUE(unknown->common.sounds.empty());

    auto unsupported_ogg = MinimalOgg(std::array<Byte, 4>{'N', 'O', 'P', 'E'});
    auto unsupported = Decode(MakeCurrentSoundFixture(unsupported_ogg));
    ASSERT_TRUE(unsupported.ok()) << unsupported.status();
    EXPECT_EQ(unsupported->omitted_sound_count, 1);
    EXPECT_TRUE(unsupported->common.sounds.empty());
}

TEST(LegacyHistoricalDecoderTest, AccountsForNonZeroOpusGranuleOrigin) {
    constexpr std::array<Byte, 19> kHead{'O', 'p', 'u', 's', 'H', 'e', 'a',
                                         'd', 1,   2,   0x38, 1,   0,   0,
                                         0,   0,   0,   0,   0};
    constexpr std::array<Byte, 16> kTags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's',
                                         0,   0,   0,   0,   0,   0,   0,   0};
    constexpr std::array<Byte, 3> kAudio{0xf8, 0xff, 0xfe};
    constexpr std::uint64_t kOrigin = 48'000;
    std::vector<Byte> bytes;
    AppendOggPage(bytes, kHead, 0x02, 0, 0, 1);
    AppendOggPage(bytes, kTags, 0, 0, 1, 1);
    AppendOggPage(bytes, kAudio, 0, kOrigin + 960, 2, 1);
    AppendOggPage(bytes, kAudio, 0x04, kOrigin + 1'800, 3, 1);

    const auto decoded = Decode(MakeCurrentSoundFixture(bytes));
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    ASSERT_EQ(decoded->common.sounds.size(), 1);
    const auto& sound = decoded->common.sounds.front().second.value;
    EXPECT_EQ(sound.encoding, SoundEncoding::kOpus);
    EXPECT_EQ(sound.sample_rate, 48'000);
    EXPECT_EQ(sound.samples, 1'488);
}

TEST(LegacyHistoricalDecoderTest, SilentlyOmitsUnknownSoundFamilies) {
    constexpr std::array<Byte, 4> kMp3Frame{0xff, 0xfb, 0x90, 0x64};
    constexpr std::array<Byte, 12> kWave{'R', 'I', 'F', 'F', 4,   0,
                                         0,   0,   'W', 'A', 'V', 'E'};
    constexpr std::array<Byte, 5> kOpaque{3, 1, 4, 1, 5};
    const std::array<std::span<const Byte>, 4> unknown{
        std::span<const Byte>{}, kMp3Frame, kWave, kOpaque};
    for (const auto bytes : unknown) {
        auto decoded = Decode(MakeCurrentSoundFixture(bytes));
        ASSERT_TRUE(decoded.ok()) << decoded.status();
        EXPECT_EQ(decoded->sound_field_count, 1);
        EXPECT_EQ(decoded->omitted_sound_count, 1);
        EXPECT_TRUE(decoded->common.sounds.empty());
    }
}

TEST(LegacyHistoricalDecoderTest, ValidatesIdentifiedOggIntegrity) {
    constexpr std::array<Byte, 8> kTruncatedOpus{'O', 'p', 'u', 's',
                                                 'H', 'e', 'a', 'd'};
    ExpectFailure(MakeCurrentSoundFixture(MinimalOgg(kTruncatedOpus)),
                  absl::StatusCode::kDataLoss);

    auto malformed_later_page = OpusIdentification();
    malformed_later_page.push_back(0);
    ExpectFailure(MakeCurrentSoundFixture(malformed_later_page),
                  absl::StatusCode::kDataLoss);

    auto ignored_crc = OpusIdentification();
    ignored_crc[22] ^= 1;
    ExpectFailure(MakeCurrentSoundFixture(ignored_crc),
                  absl::StatusCode::kDataLoss);

    auto embedded_capture = OpusIdentification();
    constexpr std::array<Byte, 4> kEmbeddedCapture{'O', 'g', 'g', 'S'};
    embedded_capture[27] += kEmbeddedCapture.size();
    embedded_capture.insert(embedded_capture.end(), kEmbeddedCapture.begin(),
                            kEmbeddedCapture.end());
    ExpectFailure(MakeCurrentSoundFixture(embedded_capture),
                  absl::StatusCode::kDataLoss);
}

TEST(LegacyHistoricalDecoderTest, PreparesRgbaAndPngAtTheImageBoundary) {
    constexpr std::array<Byte, 4> kPixel{0x10, 0x20, 0x30, 0xFF};
    LegacyImage rgba;
    rgba.bytes = BufferManaged(kPixel);
    rgba.width = 1;
    rgba.height = 1;
    ASSERT_TRUE(PrepareImage(rgba, ImageRole::kTexture).ok());
    EXPECT_EQ(rgba.role, ImageRole::kTexture);
    EXPECT_NE(rgba.encoding, ImageEncoding::kRgba);
    EXPECT_TRUE(ValidatePreparedImage(rgba, ImageRole::kTexture).ok());

    constexpr std::array<Byte, 70> kPng{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x10, 0x50, 0x30, 0xF8,
        0x0F, 0x00, 0x02, 0x04, 0x01, 0x60, 0x8D, 0xBC, 0xBB, 0x71, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    LegacyImage png;
    png.bytes = BufferManaged(kPng);
    png.width = 1;
    png.height = 1;
    png.encoding = ImageEncoding::kPng;
    ASSERT_TRUE(PrepareImage(png, ImageRole::kAvatar).ok());
    EXPECT_EQ(png.role, ImageRole::kAvatar);
    EXPECT_NE(png.encoding, ImageEncoding::kPng);
    EXPECT_NE(png.encoding, ImageEncoding::kRgba);
    EXPECT_TRUE(ValidatePreparedImage(png, ImageRole::kAvatar).ok());
#ifndef YSM_ANDROID
    EXPECT_EQ(png.encoding, ImageEncoding::kWebp);
#endif
}

TEST(LegacyHistoricalDecoderTest, EnforcesSoundLimitBeforeClassification) {
    std::vector<Byte> at_limit(4 * 1024 * 1024, 0);
    auto below_limit = at_limit;
    below_limit.pop_back();
    auto below = Decode(MakeCurrentSoundFixture(below_limit));
    ASSERT_TRUE(below.ok()) << below.status();
    EXPECT_EQ(below->omitted_sound_count, 1);

    auto omitted = Decode(MakeCurrentSoundFixture(at_limit));
    ASSERT_TRUE(omitted.ok()) << omitted.status();
    EXPECT_EQ(omitted->omitted_sound_count, 1);

    at_limit.push_back(0);
    ExpectFailure(MakeCurrentSoundFixture(at_limit),
                  absl::StatusCode::kResourceExhausted);
}

TEST(LegacyHistoricalDecoderTest, AcceptsEveryHistoricalVersion) {
    for (std::uint32_t version = 1; version <= 32; ++version) {
        SCOPED_TRACE(version);
        auto result = Decode(MakeFixture(version));
        ASSERT_TRUE(result.ok()) << result.status();
        EXPECT_EQ(result->version, version);
        EXPECT_EQ(result->info.hash, kModelHash);
    }
}

TEST(LegacyHistoricalDecoderTest, MaterializesDefaultWheelAnimations) {
    for (std::uint32_t version = 1; version <= 32; ++version) {
        SCOPED_TRACE(version);
        const auto fixture = MakeFixture(version);
        auto decoded = Decode(fixture);
        ASSERT_TRUE(decoded.ok()) << decoded.status();
        decoded->info.properties.extra_animation.clear();
        decoded->info.properties.extra_animation_classify.emplace_back();
        decoded->info.properties.extra_animation_classify.back().id = "poses";
        decoded->info.properties.extra_animation_classify.back().animations = {
            {"pose", "Pose"}};
        const auto converted = conversion::Convert(std::move(*decoded), fixture.size());
        ASSERT_TRUE(converted.ok()) << converted.status();
        const auto manifest =
            ParsePayload<proto::manifest::Manifest>(converted->payloads[0]);
        const auto& settings = manifest.info.settings;
        ASSERT_EQ(settings.extra_animation.size(), 8);
        for (std::size_t index = 0; index < 8; ++index) {
            EXPECT_EQ(settings.extra_animation[index].key,
                      "extra" + std::to_string(index));
            EXPECT_TRUE(settings.extra_animation[index].value.empty());
        }
        ASSERT_EQ(settings.extra_animation_classify.size(), 1);
        EXPECT_EQ(settings.extra_animation_classify.front().id, "poses");
    }
}

TEST(LegacyHistoricalDecoderTest, PreservesExplicitWheelAnimations) {
    const auto fixture = MakeFixture(32);
    auto decoded = Decode(fixture);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    decoded->info.properties.extra_animation = {{"wave", "Wave"}};
    const auto converted = conversion::Convert(std::move(*decoded), fixture.size());
    ASSERT_TRUE(converted.ok()) << converted.status();
    const auto manifest =
        ParsePayload<proto::manifest::Manifest>(converted->payloads[0]);
    ASSERT_EQ(manifest.info.settings.extra_animation.size(), 1);
    EXPECT_EQ(manifest.info.settings.extra_animation.front().key, "wave");
    EXPECT_EQ(manifest.info.settings.extra_animation.front().value, "Wave");
}

TEST(LegacyHistoricalDecoderTest, ConvertsSharedPlaintextConcurrently) {
    const auto fixture = MakeFixture(32);
    const auto convert = [&fixture]() -> absl::StatusOr<ImportResult> {
        auto model = Decode(fixture);
        if (!model.ok()) {
            return model.status();
        }
        return conversion::Convert(std::move(*model), fixture.size());
    };
    const auto expected = convert();
    ASSERT_TRUE(expected.ok()) << expected.status();

    std::array<std::future<absl::StatusOr<ImportResult>>, 4> imports;
    std::latch start(imports.size());
    for (auto& pending : imports) {
        pending = std::async(std::launch::async, [&convert, &start] {
            start.arrive_and_wait();
            return convert();
        });
    }
    for (auto& pending : imports) {
        const auto imported = pending.get();
        ASSERT_TRUE(imported.ok()) << imported.status();
        EXPECT_EQ(imported->metadata.model_id, expected->metadata.model_id);
        ASSERT_EQ(imported->payloads.size(), expected->payloads.size());
        for (std::size_t index = 0; index < imported->payloads.size(); ++index) {
            EXPECT_TRUE(std::ranges::equal(imported->payloads[index].bytes,
                                           expected->payloads[index].bytes));
        }
    }
}

TEST(LegacyHistoricalDecoderTest, PreservesVersionFiveFaces) {
    auto result = Decode(MakeFixture(5, kModelHash, true));
    ASSERT_TRUE(result.ok()) << result.status();
    const auto& main = result->player->geo_models.front().second.value;
    ASSERT_EQ(main.bones.size(), 1);
    ASSERT_EQ(main.bones.front().cubes.size(), 1);
    ASSERT_EQ(main.bones.front().cubes.front().quads.size(), 1);
    EXPECT_EQ(main.v5_global_cube_count, 1);
    EXPECT_FLOAT_EQ(
        main.bones.front().cubes.front().quads.front().vertices[2].texture_v,
        1.0F);
}

TEST(LegacyHistoricalDecoderTest, PreservesInfiniteAnimationLengthSentinel) {
    auto fixture = MakeFixture(32, kModelHash, false, false, {}, false, true);
    auto result = Decode(fixture);
    ASSERT_TRUE(result.ok()) << result.status();
    const auto& animations = result->player->animations.front().second.value;
    ASSERT_EQ(animations.animations.size(), 1);
    EXPECT_EQ(animations.animations.front().length,
              std::numeric_limits<double>::infinity());

    constexpr std::array<Byte, 4> kPositiveInfinity{0x00, 0x00, 0x80, 0x7F};
    const auto length =
        std::search(fixture.begin(), fixture.end(), kPositiveInfinity.begin(),
                    kPositiveInfinity.end());
    ASSERT_NE(length, fixture.end());
    auto negative_infinity = fixture;
    negative_infinity[static_cast<std::size_t>(length - fixture.begin()) + 3] =
        0xFF;
    auto negative = Decode(negative_infinity);
    ASSERT_TRUE(negative.ok()) << negative.status();
    EXPECT_EQ(negative->player->animations.front()
                  .second.value.animations.front()
                  .length,
              -std::numeric_limits<double>::infinity());
    auto nan = fixture;
    nan[static_cast<std::size_t>(length - fixture.begin())] = 1;
    auto decoded_nan = Decode(nan);
    ASSERT_TRUE(decoded_nan.ok()) << decoded_nan.status();
    EXPECT_TRUE(std::isnan(decoded_nan->player->animations.front()
                               .second.value.animations.front()
                               .length));
}

TEST(LegacyHistoricalDecoderTest, RequiresFullFloatVersionTwentyFive) {
    auto full_float = Decode(MakeFixture(25));
    ASSERT_TRUE(full_float.ok()) << full_float.status();
    EXPECT_DOUBLE_EQ(full_float->player->geo_models.front()
                         .second.value.properties.texture_height,
                     1.25);

    ExpectFailure(MakeFixture(25, kModelHash, false, true),
                  absl::StatusCode::kFailedPrecondition);
}

TEST(LegacyHistoricalDecoderTest, MapsLegacyHashWithTrailingZeroExtension) {
    auto lower = Decode(MakeFixture(32));
    auto upper = Decode(MakeFixture(32, "000102030405060708090A0B0C0D0E0F"));
    ASSERT_TRUE(lower.ok());
    ASSERT_TRUE(upper.ok());
    EXPECT_EQ(lower->model_id, upper->model_id);
    for (std::size_t index = 0; index < 16; ++index) {
        EXPECT_EQ(lower->model_id[index], index);
        EXPECT_EQ(lower->model_id[index + 16], 0);
    }
    for (std::size_t index = 0; index < kModelHash.size(); ++index) {
        auto changed_hash = std::string(kModelHash);
        changed_hash[index] = changed_hash[index] == 'f' ? 'e' : 'f';
        auto changed = Decode(MakeFixture(32, changed_hash));
        ASSERT_TRUE(changed.ok());
        EXPECT_NE(changed->model_id, lower->model_id) << index;
    }
}

TEST(LegacyHistoricalDecoderTest,
     RejectsMalformedHashAndWireButPreservesBusinessAnomalies) {
    ExpectFailure(MakeFixture(32, "000102030405060708090a0b0c0d0e"),
                  absl::StatusCode::kDataLoss);
    ExpectFailure(MakeFixture(32, "000102030405060708090a0b0c0d0e0g"),
                  absl::StatusCode::kDataLoss);
    auto dangling =
        Decode(MakeFixture(32, kModelHash, false, false, "missing"));
    ASSERT_TRUE(dangling.ok()) << dangling.status();
    EXPECT_EQ(
        dangling->player->geo_models.front().second.value.bones.front().parent,
        "missing");
    auto cyclic = Decode(MakeFixture(32, kModelHash, false, false, "main"));
    ASSERT_TRUE(cyclic.ok()) << cyclic.status();
    EXPECT_EQ(
        cyclic->player->geo_models.front().second.value.bones.front().parent,
        "main");
    auto nonfinite =
        Decode(MakeFixture(32, kModelHash, false, false, {}, true));
    ASSERT_TRUE(nonfinite.ok()) << nonfinite.status();
    EXPECT_TRUE(std::isnan(nonfinite->player->geo_models.front()
                               .second.value.properties.texture_height));

    auto trailing = MakeFixture(32);
    trailing.push_back(0);
    ExpectFailure(trailing, absl::StatusCode::kDataLoss);

    auto truncated = MakeFixture(32);
    truncated.pop_back();
    ExpectFailure(truncated, absl::StatusCode::kDataLoss);
}

TEST(LegacyHistoricalDecoderTest, RejectsUnsupportedVersionAndBudgets) {
    FixtureWriter zero;
    zero.FixedU32(0);
    ExpectFailure(std::move(zero).Finish(), absl::StatusCode::kUnimplemented);

    FixtureWriter unsupported;
    unsupported.FixedU32(33);
    ExpectFailure(std::move(unsupported).Finish(),
                  absl::StatusCode::kUnimplemented);

    FixtureWriter oversized;
    oversized.FixedU32(1);
    oversized.VarU32(1'048'577);  // feature collection
    ExpectFailure(std::move(oversized).Finish(),
                  absl::StatusCode::kResourceExhausted);
}

TEST(LegacyHistoricalDecoderTest, RejectsInvalidTagsEnumsAndDuplicateKeys) {
    FixtureWriter invalid_enum;
    invalid_enum.FixedU32(1);
    invalid_enum.VarU32(1);  // features
    invalid_enum.VarU32(4);
    ExpectFailure(std::move(invalid_enum).Finish(),
                  absl::StatusCode::kDataLoss);

    FixtureWriter invalid_optional;
    invalid_optional.FixedU32(1);
    invalid_optional.VarU32(0);  // features
    invalid_optional.VarU32(1);  // geos
    invalid_optional.VarU32(1);  // main
    invalid_optional.VarU32(2);  // invalid optional tag
    ExpectFailure(std::move(invalid_optional).Finish(),
                  absl::StatusCode::kDataLoss);

    FixtureWriter duplicate;
    duplicate.FixedU32(1);
    duplicate.VarU32(0);  // features
    duplicate.VarU32(2);  // geos
    duplicate.VarU32(1);
    duplicate.VarU32(1);
    WriteGeo(duplicate, 1, "main");
    duplicate.VarU32(1);  // duplicate key is rejected before its value
    ExpectFailure(std::move(duplicate).Finish(),
                  absl::StatusCode::kFailedPrecondition);

    FixtureWriter noncanonical;
    noncanonical.FixedU32(1);
    noncanonical.Bytes(std::array<Byte, 2>{0x80, 0x00});
    auto bytes = std::move(noncanonical).Finish();
    bytes.erase(bytes.begin() + 4);  // expose 0x80,0x00 as feature count
    ExpectFailure(bytes, absl::StatusCode::kDataLoss);

    auto invalid_utf8 = MakeFixture(32);
    constexpr std::string_view kIdentifier = "geometry.test";
    const auto identifier =
        std::search(invalid_utf8.begin(), invalid_utf8.end(),
                    kIdentifier.begin(), kIdentifier.end());
    ASSERT_NE(identifier, invalid_utf8.end());
    *identifier = 0xFF;
    ExpectFailure(invalid_utf8, absl::StatusCode::kFailedPrecondition);
}

TEST(LegacyHistoricalDecoderTest,
     ProjectsCompleteDeterministicPayloadAndPreservesBusinessAnomalies) {
    auto fixture =
        MakeFixture(32, kModelHash, false, false, "missing", false, true);
    auto decoded = Decode(fixture);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    auto& animation =
        decoded->player->animations.front().second.value.animations.front();
    animation.start_delay = MolangValue{MolangValueType::kString, 0, "delay"};
    animation.loop_delay = MolangValue{MolangValueType::kDouble, 1.5, {}};
    animation.override_previous = false;
    animation.instructions.push_back(
        InstructionKeyFrame{{"v.first=1", "v.second=2;"}, 1.0});
    animation.sounds = {{"event-b", 2.0}, {"", 2.0}};

    AnimationController controller;
    controller.initial_state = "missing";
    ControllerState state;
    state.animations = {{"idle", ""}};
    state.transitions = {{"also-missing", "query.ok"}};
    state.on_entry = {"v.entered=1", "v.ready=1;"};
    controller.states.emplace_back("default", std::move(state));
    AnimationControllerFile controller_file;
    controller_file.controllers.emplace_back("controller",
                                             std::move(controller));
    decoded->player->controllers.emplace_back(
        "player", WithSourceHash<AnimationControllerFile>{
                      "", std::move(controller_file)});

    const auto pixel = decoded->player->textures.front().second.uv.value;
    decoded->info.metadata =
        ModelMetadata{"model",
                      "tips",
                      {},
                      {ModelAuthor{"Alice", "author", {}, ""},
                       ModelAuthor{"Alice", "maintainer", {}, ""}},
                      {}};
    auto avatar = pixel;
    ASSERT_TRUE(PrepareImage(avatar, ImageRole::kAvatar).ok());
    decoded->info.author_avatars.emplace_back("Alice", std::move(avatar));
    auto gui = pixel;
    ASSERT_TRUE(PrepareImage(gui, ImageRole::kGui).ok());
    decoded->info.gui_images.emplace_back("gui_foreground", std::move(gui));
    auto icon = pixel;
    ASSERT_TRUE(PrepareImage(icon, ImageRole::kIcon).ok());
    decoded->info.gui_images.emplace_back("thumb-icon", std::move(icon));
    ExtraAnimationButton inert_button;
    inert_button.id = "inert";
    inert_button.forms.emplace_back();
    inert_button.forms.front().type = "checkbox";
    decoded->info.properties.extra_animation_buttons.emplace_back(
        std::move(inert_button));
    decoded->common.user_functions.emplace_back(
        "z", WithSourceHash<std::string>{"", "z-content"});
    decoded->common.user_functions.emplace_back(
        "a", WithSourceHash<std::string>{"", "a-content"});
    LanguageFile language;
    language.entries = {{"z", "last"}, {"a", "first"}};
    decoded->common.languages.emplace_back(
        "en_us", WithSourceHash<LanguageFile>{"", std::move(language)});
    const auto vorbis = VorbisIdentification();
    const auto opus = OpusIdentification();
    decoded->common.sounds.emplace_back(
        "z.ogg", WithSourceHash<LegacySound>{
                     "", LegacySound{BufferManaged(vorbis),
                                     SoundEncoding::kVorbis, 2, 44'100, 960}});
    decoded->common.sounds.emplace_back(
        "a.ogg", WithSourceHash<LegacySound>{
                     "", LegacySound{BufferManaged(opus), SoundEncoding::kOpus,
                                     2, 48'000, 960}});
    ReplaceModel projectile;
    projectile.source_key = "arrow";
    projectile.match = {"minecraft:arrow", "minecraft:arrow"};
    decoded->projectiles.emplace_back(std::move(projectile));

    auto projected = conversion::Convert(*decoded, fixture.size());
    ASSERT_TRUE(projected.ok()) << projected.status();
    const auto& result = *projected;
    ASSERT_EQ(result.payloads.size(), 11);
    EXPECT_EQ(result.metadata.inner_version, 32);
    EXPECT_EQ(result.metadata.source_size, fixture.size());
    EXPECT_TRUE(std::equal(decoded->model_id.begin(), decoded->model_id.end(),
                           result.metadata.model_id.begin()));

    const auto manifest =
        ParsePayload<proto::manifest::Manifest>(result.payloads[0]);
    EXPECT_TRUE(manifest.common_assets.sounds.empty());
    ASSERT_EQ(manifest.render_targets.size(), 2);
    EXPECT_EQ(manifest.render_targets[0].target_id, "player");
    EXPECT_EQ(manifest.render_targets[1].target_id, "projectile-1");
    EXPECT_EQ(manifest.render_targets[1].match,
              (std::vector<std::string>{"minecraft:arrow", "minecraft:arrow"}));
    EXPECT_EQ(
        manifest.info.properties.model_id,
        std::string(reinterpret_cast<const char*>(decoded->model_id.data()),
                    decoded->model_id.size()));
    ASSERT_TRUE(manifest.info.icon_source.has_value());
    EXPECT_EQ(*manifest.info.icon_source,
              proto::manifest::info::PreviewSource::kRaw);
    EXPECT_FALSE(manifest.info.thumbnail_source.has_value());
    ASSERT_EQ(manifest.info.settings.extra_animation_buttons.size(), 1);
    const auto& inert_form =
        manifest.info.settings.extra_animation_buttons.front()
            .config_forms.front();
    ASSERT_EQ(inert_form.read_program.payload.index(), 1);
    EXPECT_EQ(std::get<1>(inert_form.read_program.payload), "0");
    ASSERT_EQ(inert_form.write_program.payload.index(), 1);
    EXPECT_EQ(std::get<1>(inert_form.write_program.payload), "return;");
    ASSERT_EQ(manifest.info.language_files.size(), 1);
    ASSERT_EQ(manifest.info.language_files.front().entries.size(), 2);
    EXPECT_EQ(manifest.info.language_files.front().entries[0].key, "a");
    EXPECT_EQ(manifest.info.language_files.front().entries[1].key, "z");

    const auto strings =
        ParsePayload<proto::asset::strings::StringData>(result.payloads[1]);
    ASSERT_EQ(strings.user_functions.size(), 2);
    EXPECT_EQ(strings.user_functions[0].name, "a");
    EXPECT_EQ(strings.user_functions[1].name, "z");
    ASSERT_EQ(strings.user_functions[0].body.payload.index(), 1);
    EXPECT_EQ(std::get<1>(strings.user_functions[0].body.payload), "a-content");
    EXPECT_EQ(strings.user_functions[0].body.format, 0);

    const auto player =
        ParsePayload<proto::asset::model::ModelData>(result.payloads[2]);
    ASSERT_EQ(player.geo_models.size(), 2);
    const auto main_entry = std::ranges::find_if(
        player.geo_models,
        [](const auto& entry) { return entry.key == "main"; });
    ASSERT_NE(main_entry, player.geo_models.end());
    const auto& main_geo = main_entry->value;
    ASSERT_EQ(main_geo.bones.size(), 1);
    EXPECT_EQ(main_geo.bones.front().parent, "missing");
    ASSERT_EQ(player.animation_files.size(), 1);
    ASSERT_EQ(player.animation_files.front().value.animations.size(), 1);
    const auto& projected_animation =
        player.animation_files.front().value.animations.front();
    EXPECT_EQ(projected_animation.length,
              std::numeric_limits<float>::infinity());
    ASSERT_EQ(projected_animation.instruction_keyframes.size(), 1);
    EXPECT_EQ(
        std::get<1>(
            projected_animation.instruction_keyframes.front().programs.payload),
        "v.first=1;\nv.second=2;\n");
    ASSERT_EQ(projected_animation.sound_keyframes.size(), 2);
    EXPECT_EQ(projected_animation.sound_keyframes[0].data, "event-b");
    EXPECT_EQ(projected_animation.sound_keyframes[1].data, "");
    ASSERT_EQ(player.animation_controllers.size(), 1);
    const auto& projected_controller =
        player.animation_controllers.front().value.controllers.front();
    EXPECT_EQ(projected_controller.default_state, "missing");
    const auto& projected_state = projected_controller.states.front();
    ASSERT_EQ(projected_state.animations.size(), 1);
    EXPECT_EQ(std::get<1>(projected_state.animations.front().condition.payload),
              "true");
    ASSERT_TRUE(projected_state.on_entry.has_value());
    EXPECT_EQ(std::get<1>(projected_state.on_entry->payload),
              "v.entered=1;\nv.ready=1;\n");
    EXPECT_EQ(projected_state.transitions.front().dst, "also-missing");
    EXPECT_EQ(
        std::get<1>(projected_state.transitions.front().condition.payload),
        "query.ok");

    for (std::size_t index = 4; index < 8; ++index) {
        EXPECT_EQ(result.payloads[index].kind, PayloadKind::kBlobImage);
        EXPECT_NE(result.payloads[index].encoding, PayloadEncoding::kRgba);
    }
    EXPECT_EQ(result.payloads[6].bytes.size(), result.payloads[7].bytes.size());
    EXPECT_TRUE(std::equal(
        result.payloads[6].bytes.data(),
        result.payloads[6].bytes.data() + result.payloads[6].bytes.size(),
        result.payloads[7].bytes.data()));
    EXPECT_EQ(result.payloads[8].kind, PayloadKind::kNamedImage);
    EXPECT_EQ(result.payloads[8].name, "thumb-icon");
    EXPECT_EQ(result.payloads[9].name, "a.ogg");
    EXPECT_EQ(result.payloads[10].name, "z.ogg");
    ASSERT_TRUE(result.payloads[9].sound.has_value());
    EXPECT_EQ(result.payloads[9].sound->channels, 2);
    EXPECT_EQ(result.payloads[9].sound->sample_rate, 48'000);
    EXPECT_EQ(result.payloads[9].sound->frames, 960);
    ASSERT_TRUE(result.payloads[10].sound.has_value());
    EXPECT_EQ(result.payloads[10].sound->channels, 2);
    EXPECT_EQ(result.payloads[10].sound->sample_rate, 44'100);
    EXPECT_EQ(result.payloads[10].sound->frames, 960);

    auto repeated = conversion::Convert(*decoded, fixture.size());
    ASSERT_TRUE(repeated.ok()) << repeated.status();
    EXPECT_EQ(repeated->metadata.model_id, result.metadata.model_id);
    ASSERT_EQ(repeated->payloads.size(), result.payloads.size());
    for (std::size_t index = 0; index < result.payloads.size(); ++index) {
        const auto& left = result.payloads[index];
        const auto& right = repeated->payloads[index];
        EXPECT_EQ(left.kind, right.kind);
        EXPECT_EQ(left.encoding, right.encoding);
        EXPECT_EQ(left.logical_id, right.logical_id);
        EXPECT_EQ(left.name, right.name);
        EXPECT_EQ(left.bytes.size(), right.bytes.size());
        EXPECT_TRUE(std::equal(left.bytes.data(),
                               left.bytes.data() + left.bytes.size(),
                               right.bytes.data()));
    }
}

TEST(LegacyHistoricalDecoderTest, ProjectsSharedCubeAttributesOnce) {
    auto decoded = Decode(MakeFixture(32));
    ASSERT_TRUE(decoded.ok()) << decoded.status();

    constexpr std::array<Vec3, 8> kPositions{{
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
        {0, 0, 1},
        {1, 0, 1},
        {1, 1, 1},
        {0, 1, 1},
    }};
    constexpr std::array<std::array<std::uint32_t, 4>, 6> kFaces{{
        {0, 1, 2, 3},
        {4, 5, 6, 7},
        {0, 3, 7, 4},
        {1, 5, 6, 2},
        {3, 2, 6, 7},
        {0, 4, 5, 1},
    }};
    constexpr std::array<Vec3, 6> kNormals{{
        {0, 0, -1},
        {0, 0, 1},
        {-1, 0, 0},
        {1, 0, 0},
        {0, 1, 0},
        {0, -1, 0},
    }};

    Cube source_cube;
    for (std::size_t face = 0; face < kFaces.size(); ++face) {
        Quad quad;
        quad.normal = kNormals[face];
        for (std::size_t vertex = 0; vertex < quad.vertices.size(); ++vertex) {
            quad.vertices[vertex].position = kPositions[kFaces[face][vertex]];
            quad.vertices[vertex].texture_u = static_cast<float>(vertex % 2);
            quad.vertices[vertex].texture_v = static_cast<float>(vertex / 2);
        }
        source_cube.quads.emplace_back(std::move(quad));
    }
    auto second_cube = source_cube;
    decoded->player->geo_models.front()
        .second.value.bones.front()
        .cubes.emplace_back(std::move(source_cube));
    decoded->player->geo_models.front()
        .second.value.bones.front()
        .cubes.emplace_back(std::move(second_cube));

    auto projected = conversion::Convert(*decoded, 1);
    ASSERT_TRUE(projected.ok()) << projected.status();
    const auto player =
        ParsePayload<proto::asset::model::ModelData>(projected->payloads[2]);
    const auto main_entry = std::ranges::find_if(
        player.geo_models,
        [](const auto& entry) { return entry.key == "main"; });
    ASSERT_NE(main_entry, player.geo_models.end());
    const auto& main_geo = main_entry->value;
    ASSERT_EQ(main_geo.cubes.cubes_legacy.size(), 2);
    for (const auto& cube : main_geo.cubes.cubes_legacy) {
        EXPECT_EQ(cube.faceCount, kFaces.size());
        ASSERT_EQ(cube.pos.size(), kPositions.size() * 3);
        ASSERT_EQ(cube.pos_indices.size(), kFaces.size() * 4);
        ASSERT_EQ(cube.uv.size(), 8);
        ASSERT_EQ(cube.uv_indices.size(), kFaces.size() * 4);
        for (std::size_t face = 0; face < kFaces.size(); ++face) {
            for (std::size_t vertex = 0; vertex < kFaces[face].size();
                 ++vertex) {
                const auto projected_vertex = face * 4 + vertex;
                EXPECT_EQ(cube.pos_indices[projected_vertex],
                          kFaces[face][vertex]);
                EXPECT_EQ(cube.uv_indices[projected_vertex], vertex);
            }
        }
    }
}

TEST(LegacyHistoricalDecoderTest,
     RejectsPayloadRecordOverflowBeforeOutputConstruction) {
    LegacyModel targets;
    targets.version = 32;
    targets.projectiles.resize(kMaxPayloadCount - 1);
    auto target_result = conversion::Convert(targets, 1);
    ASSERT_FALSE(target_result.ok());
    EXPECT_EQ(target_result.status().code(),
              absl::StatusCode::kResourceExhausted);

    LegacyModel images;
    images.version = 32;
    images.player.emplace();
    images.player->textures.reserve(kMaxPayloadCount - 2);
    for (std::size_t index = 0; index < kMaxPayloadCount - 2; ++index) {
        TextureSet texture;
        texture.uv.value.width = 1;
        texture.uv.value.height = 1;
        images.player->textures.emplace_back(std::to_string(index),
                                             std::move(texture));
    }
    auto image_result = conversion::Convert(images, 1);
    ASSERT_FALSE(image_result.ok());
    EXPECT_EQ(image_result.status().code(),
              absl::StatusCode::kResourceExhausted);

    LegacyModel sounds;
    sounds.version = 32;
    sounds.common.sounds.reserve(kMaxPayloadCount - 1);
    for (std::size_t index = 0; index < kMaxPayloadCount - 1; ++index) {
        sounds.common.sounds.emplace_back(
            std::to_string(index),
            WithSourceHash<LegacySound>{"", LegacySound{BufferManaged{}}});
    }
    auto sound_result = conversion::Convert(sounds, 1);
    ASSERT_FALSE(sound_result.ok());
    EXPECT_EQ(sound_result.status().code(),
              absl::StatusCode::kResourceExhausted);
}

TEST(LegacyHistoricalDecoderTest, ClassifiesTargetRepresentationFailures) {
    auto base = Decode(MakeFixture(32));
    ASSERT_TRUE(base.ok()) << base.status();

    auto invalid_enum = *base;
    invalid_enum.player->geo_models.front().first =
        static_cast<PlayerModelType>(99);
    auto enum_result = conversion::Convert(invalid_enum, 1);
    ASSERT_FALSE(enum_result.ok());
    EXPECT_EQ(enum_result.status().code(),
              absl::StatusCode::kFailedPrecondition);

    auto float_overflow = *base;
    ExtraAnimationButton button;
    button.forms.emplace_back();
    button.forms.front().value = "v.value";
    button.forms.front().step = (std::numeric_limits<double>::max)();
    float_overflow.info.properties.extra_animation_buttons.emplace_back(
        std::move(button));
    auto float_result = conversion::Convert(float_overflow, 1);
    ASSERT_FALSE(float_result.ok());
    EXPECT_EQ(float_result.status().code(),
              absl::StatusCode::kFailedPrecondition);

    auto image_conflict = *base;
    image_conflict.player->textures.front().second.uv.value.encoding =
        ImageEncoding::kPng;
    auto image_result = conversion::Convert(image_conflict, 1);
    ASSERT_FALSE(image_result.ok());
    EXPECT_EQ(image_result.status().code(),
              absl::StatusCode::kFailedPrecondition);

    auto target_collision = *base;
    target_collision.version = 26;
    target_collision.projectiles.clear();
    target_collision.projectiles.emplace_back(ReplaceModel{"same"});
    target_collision.projectiles.emplace_back(ReplaceModel{"same"});
    auto collision_result = conversion::Convert(target_collision, 1);
    ASSERT_FALSE(collision_result.ok());
    EXPECT_EQ(collision_result.status().code(),
              absl::StatusCode::kFailedPrecondition);
}

TEST(LegacyHistoricalDecoderTest, HandlesDeterministicMutationSmoke) {
    std::mt19937 generator(0x59534D33U);
    std::uniform_int_distribution<std::size_t> length_distribution(0, 512);
    std::uniform_int_distribution<unsigned int> byte_distribution(0, 255);
    for (std::size_t iteration = 0; iteration < 512; ++iteration) {
        std::vector<Byte> bytes(length_distribution(generator));
        for (auto& byte : bytes) {
            byte = static_cast<Byte>(byte_distribution(generator));
        }
        EXPECT_NO_THROW(static_cast<void>(Decode(bytes)));
    }

    auto fixture = MakeFixture(32);
    for (std::size_t offset = 0; offset < fixture.size(); ++offset) {
        auto mutated = fixture;
        mutated[offset] ^= static_cast<Byte>(1U << (offset % 8U));
        EXPECT_NO_THROW(static_cast<void>(Decode(mutated))) << offset;
    }
}
}  // namespace
}  // namespace ysm::legacy::v3::container
