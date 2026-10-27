#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <legacy/result_owner.h>

namespace ysm::legacy::java {
namespace {
Payload MakePayload(PayloadKind kind, PayloadEncoding encoding,
                    std::uint32_t logical_id, std::string name,
                    std::string_view bytes = {},
                    std::optional<ImageMetadata> image = std::nullopt,
                    std::optional<v3::SoundMetadata> sound = std::nullopt) {
    return Payload{kind,       encoding,
                   logical_id, std::move(name),
                   image,      BufferManaged(StrBuf(bytes)),
                   sound};
}

ImportResult MinimalResult() {
    std::vector<Payload> payloads;
    payloads.emplace_back(MakePayload(
        PayloadKind::kManifest, PayloadEncoding::kDirect, 0, "", "manifest"));
    payloads.emplace_back(
        MakePayload(PayloadKind::kStringData, PayloadEncoding::kDirect, 1, ""));
    payloads.emplace_back(MakePayload(PayloadKind::kModelData,
                                      PayloadEncoding::kDirect, 2, "player",
                                      "model"));
    return ImportResult{{17, 1234, {}}, std::move(payloads)};
}

std::uint32_t ReadU32(BufferViewR data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           static_cast<std::uint32_t>(data[offset + 1]) << 8U |
           static_cast<std::uint32_t>(data[offset + 2]) << 16U |
           static_cast<std::uint32_t>(data[offset + 3]) << 24U;
}

std::uint64_t ReadU64(BufferViewR data, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data[offset + index])
                 << (index * 8U);
    }
    return value;
}

TEST(LegacyResultProtocolTest, EncodesExactUnversionedDescriptor) {
    auto result = MinimalResult();
    for (std::size_t index = 0; index < result.metadata.model_id.size();
         ++index) {
        result.metadata.model_id[index] = static_cast<Byte>(index);
    }

    const auto descriptor = EncodeDescriptor(result);
    ASSERT_TRUE(descriptor.ok()) << descriptor.status();
    ASSERT_EQ(descriptor->size(), 60 + 40 * 3 + 6);
    EXPECT_EQ(
        std::string_view(reinterpret_cast<const char*>(descriptor->data()), 8),
        std::string_view("YSMLEGI\0", 8));
    EXPECT_EQ(ReadU32(*descriptor, 8), 17);
    EXPECT_EQ(ReadU32(*descriptor, 12), 3);
    EXPECT_EQ(ReadU32(*descriptor, 16), 0);
    EXPECT_EQ(ReadU64(*descriptor, 20), 1234);
    EXPECT_TRUE(std::equal(result.metadata.model_id.begin(),
                           result.metadata.model_id.end(),
                           descriptor->begin() + 28));
}

TEST(LegacyResultProtocolTest, AcceptsZtxAndRejectsRgbaImagePayloads) {
    auto result = MinimalResult();
    result.payloads.emplace_back(MakePayload(PayloadKind::kBlobImage,
                                             PayloadEncoding::kZtx, 3, "",
                                             "ztx", ImageMetadata{1, 1, 1}));
    EXPECT_TRUE(EncodeDescriptor(result).ok());

    result.payloads.back().encoding = PayloadEncoding::kRgba;
    EXPECT_EQ(EncodeDescriptor(result).status().code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(static_cast<std::uint16_t>(PayloadEncoding::kZtx), 8);
}

TEST(LegacyResultProtocolTest, EnforcesCanonicalSoundOrderAndIds) {
    auto result = MinimalResult();
    result.payloads.emplace_back(MakePayload(PayloadKind::kSoundStream,
                                             PayloadEncoding::kOggVorbis, 1,
                                             "a.ogg", "ogg", std::nullopt,
                                             v3::SoundMetadata{2, 44'100, 960}));
    result.payloads.emplace_back(MakePayload(PayloadKind::kSoundStream,
                                             PayloadEncoding::kOggOpus, 2,
                                             "z.ogg", "ogg", std::nullopt,
                                             v3::SoundMetadata{2, 48'000, 960}));
    EXPECT_TRUE(EncodeDescriptor(result).ok());

    result.payloads.back().logical_id = 3;
    EXPECT_EQ(EncodeDescriptor(result).status().code(),
              absl::StatusCode::kInvalidArgument);
    result.payloads.back().logical_id = 2;
    std::swap(result.payloads[result.payloads.size() - 2],
              result.payloads.back());
    EXPECT_EQ(EncodeDescriptor(result).status().code(),
              absl::StatusCode::kInvalidArgument);
}

TEST(LegacyResultProtocolTest, RejectsNoncanonicalOrderAndInvalidUtf8) {
    auto result = MinimalResult();
    std::swap(result.payloads[1], result.payloads[2]);
    EXPECT_EQ(EncodeDescriptor(result).status().code(),
              absl::StatusCode::kInvalidArgument);

    result = MinimalResult();
    result.payloads[2].name = std::string("\xED\xA0\x80", 3);
    EXPECT_EQ(EncodeDescriptor(result).status().code(),
              absl::StatusCode::kInvalidArgument);
}

TEST(LegacyResultProtocolTest, OwnerKeepsPayloadBackingAndEmptySentinel) {
    auto owner = ResultOwner::Create(MinimalResult());
    ASSERT_TRUE(owner.ok()) << owner.status();
    ASSERT_NE(owner->get(), nullptr);
    ASSERT_EQ((*owner)->payloads().size(), 3);
    EXPECT_NE((*owner)->direct_address((*owner)->payloads()[1]), nullptr);
    EXPECT_TRUE((*owner)->payloads()[1].bytes.empty());
}
}  // namespace
}  // namespace ysm::legacy::java
