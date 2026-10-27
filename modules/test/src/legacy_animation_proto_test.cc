#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>
#include <ylt/struct_pb.hpp>

#include "proto/asset/model/data/animation.proto.h"
#include "proto/asset/model/data/geo_model.proto.h"

namespace ysm::test {
namespace {

namespace animation_proto = proto::asset::model::data;
namespace common_proto = proto::common;
using namespace std::string_view_literals;

struct BytesGeoModel : public iguana::base_impl<BytesGeoModel> {
    std::vector<animation_proto::Bone> bones;
    animation_proto::GeoProperties properties;
    std::string cubes;
};
YLT_REFL(BytesGeoModel, bones, properties, cubes);

constexpr std::string_view kFrozenFieldSevenWire =
    "\x3A\x0C\x0A\x0A"
    "\xE6\xA8\xA1\xE5\x9E\x8B\x3A\xE9\x93\x83"
    "\x3A\x05\x15\x00\x00\xA0\x3F"
    "\x3A\x05\x15\x00\x00\xC0\x7F"sv;

TEST(LegacyAnimationProtoTest, MatchesFrozenFieldSevenWireAndOrder) {
    animation_proto::Animation animation{};
    animation.length = 0.0F;
    animation.sound_keyframes.emplace_back(
        "\xE6\xA8\xA1\xE5\x9E\x8B\x3A\xE9\x93\x83", 0.0F);
    animation.sound_keyframes.emplace_back("", 1.25F);
    animation.sound_keyframes.emplace_back(
        "", std::numeric_limits<float>::quiet_NaN());

    std::string encoded;
    struct_pb::to_pb(animation, encoded);

    EXPECT_EQ(kFrozenFieldSevenWire, encoded);
    animation_proto::Animation parsed{};
    ASSERT_NO_THROW(parsed.from_pb(encoded));
    ASSERT_EQ(parsed.sound_keyframes.size(), 3);
    EXPECT_EQ(parsed.sound_keyframes[0].data,
              "\xE6\xA8\xA1\xE5\x9E\x8B\x3A\xE9\x93\x83");
    EXPECT_EQ(parsed.sound_keyframes[1].data, "");
    EXPECT_FLOAT_EQ(parsed.sound_keyframes[0].start_tick, 0.0F);
    EXPECT_FLOAT_EQ(parsed.sound_keyframes[1].start_tick, 1.25F);
    EXPECT_EQ(parsed.sound_keyframes[2].data, "");
    EXPECT_TRUE(std::isnan(parsed.sound_keyframes[2].start_tick));
}

TEST(LegacyAnimationProtoTest, ReadsPreFieldSevenPayloadAsAnEmptyList) {
    animation_proto::Animation current{};
    ASSERT_NO_THROW(current.from_pb(std::string("\x0A\x05sound", 7)));
    EXPECT_EQ(current.name, "sound");
    EXPECT_TRUE(current.sound_keyframes.empty());
}

TEST(LegacyAnimationProtoTest, PreservesZeroLiteralOneofPresence) {
    animation_proto::ExpressionValue source{};
    source.value = 0.0F;

    std::string encoded;
    struct_pb::to_pb(source, encoded);

    EXPECT_EQ(std::string("\x0D\x00\x00\x00\x00", 5), encoded);
    animation_proto::ExpressionValue parsed{};
    ASSERT_NO_THROW(parsed.from_pb(encoded));
    ASSERT_TRUE(std::holds_alternative<float>(parsed.value));
    EXPECT_FLOAT_EQ(std::get<float>(parsed.value), 0.0F);
}

TEST(LegacyAnimationProtoTest, PreservesProgramPayloadOneofTags) {
    common_proto::Program source{};
    source.payload.emplace<1>("q.life");

    std::string encoded;
    struct_pb::to_pb(source, encoded);
    EXPECT_EQ(std::string("\x1A\x06q.life", 8), encoded);

    common_proto::Program parsed_source{};
    ASSERT_NO_THROW(parsed_source.from_pb(encoded));
    ASSERT_EQ(parsed_source.payload.index(), 1);
    EXPECT_EQ(std::get<1>(parsed_source.payload), "q.life");

    common_proto::Program bytecode{};
    bytecode.format = 1;
    bytecode.payload.emplace<0>(std::string_view("\x00\xFF", 2));
    encoded.clear();
    struct_pb::to_pb(bytecode, encoded);
    EXPECT_EQ(std::string("\x08\x01\x12\x02\x00\xFF", 6), encoded);

    common_proto::Program parsed_bytecode{};
    ASSERT_NO_THROW(parsed_bytecode.from_pb(encoded));
    ASSERT_EQ(parsed_bytecode.payload.index(), 0);
    EXPECT_EQ(std::get<0>(parsed_bytecode.payload),
              std::string_view("\x00\xFF", 2));
}

TEST(LegacyAnimationProtoTest, PreservesStructuredCubesBytesWire) {
    animation_proto::GeoModel structured{};
    structured.properties.texture_height = 64.0F;
    structured.properties.texture_width = 64.0F;
    structured.cubes.cubes_legacy.emplace_back();
    structured.cubes.cubes_legacy.front().faceCount = 1;

    std::string encoded;
    struct_pb::to_pb(structured, encoded);

    BytesGeoModel bytes{};
    ASSERT_NO_THROW(bytes.from_pb(encoded));
    std::string expected_cubes;
    struct_pb::to_pb(structured.cubes, expected_cubes);
    EXPECT_EQ(bytes.cubes, expected_cubes);

    animation_proto::GeoModel round_trip{};
    ASSERT_NO_THROW(round_trip.from_pb(encoded));
    ASSERT_EQ(round_trip.cubes.cubes_legacy.size(), 1);
    EXPECT_EQ(round_trip.cubes.cubes_legacy.front().faceCount, 1);
}

TEST(LegacyAnimationProtoTest, PreservesSoundFrameValuesAndOrder) {
    animation_proto::Animation source{};
    source.length = 0.0F;
    source.loop = animation_proto::LoopType::kUnspecified;
    source.sound_keyframes.emplace_back("minecraft:bell", 1.25F);
    source.sound_keyframes.emplace_back(
        "\xE6\xA8\xA1\xE5\x9E\x8B\x3A\xE9\x93\x83", 1.25F);

    std::string encoded;
    struct_pb::to_pb(source, encoded);
    animation_proto::Animation parsed{};
    ASSERT_NO_THROW(parsed.from_pb(encoded));

    ASSERT_EQ(parsed.sound_keyframes.size(), 2);
    EXPECT_EQ(parsed.sound_keyframes[0].data, "minecraft:bell");
    EXPECT_EQ(parsed.sound_keyframes[1].data,
              "\xE6\xA8\xA1\xE5\x9E\x8B\x3A\xE9\x93\x83");
    EXPECT_FLOAT_EQ(parsed.sound_keyframes[0].start_tick, 1.25F);
    EXPECT_FLOAT_EQ(parsed.sound_keyframes[1].start_tick, 1.25F);
}

}  // namespace
}  // namespace ysm::test
