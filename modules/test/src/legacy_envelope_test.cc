#include "fixture_directory.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <future>
#include <latch>
#include <random>
#include <span>
#include <vector>

#include <fs.h>
#include <legacy/v3.h>
#include <string_cvt.h>

#include <v3/codec/envelope.h>

namespace ysm::legacy::v3::codec {
namespace {
void ExpectStatus(std::span<const std::uint8_t> bytes,
                  absl::StatusCode status) {
    const auto result = Import(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), status) << result.status();
}

TEST(LegacyEnvelopeTest, PublicSeamClaimsOnlyEncryptedV3) {
    constexpr std::array<std::uint8_t, 8> kRawV1{'Y', 'S', 'G', 'P',
                                                 0,   0,   0,   1};
    constexpr std::array<std::uint8_t, 8> kRawV2{'Y', 'S', 'G', 'P',
                                                 0,   0,   0,   2};
    constexpr std::array<std::uint8_t, 8> kRawV4{'Y', 'S', 'G', 'P',
                                                 0,   0,   0,   4};
    constexpr std::array<std::uint8_t, 8> kOther{'n', 'o', 't', ' ',
                                                 'y', 's', 'm', '!'};
    ExpectStatus({}, absl::StatusCode::kInvalidArgument);
    ExpectStatus(kRawV1, absl::StatusCode::kInvalidArgument);
    ExpectStatus(kRawV2, absl::StatusCode::kInvalidArgument);
    ExpectStatus(kRawV4, absl::StatusCode::kInvalidArgument);
    ExpectStatus(kOther, absl::StatusCode::kInvalidArgument);

    constexpr std::array<std::uint8_t, 12> kEncryptedV4{
        0xEF, 0xBB, 0xBF, 'Y', 'S', 'G', 'P', 0, 4, 0, 0, 0};
    ExpectStatus(kEncryptedV4, absl::StatusCode::kUnimplemented);
}

TEST(LegacyEnvelopeTest, RejectsTruncatedEncryptedEnvelope) {
    constexpr std::array<std::uint8_t, 11> kTruncated{
        0xEF, 0xBB, 0xBF, 'Y', 'S', 'G', 'P', 0, 3, 0, 0};
    ExpectStatus(kTruncated, absl::StatusCode::kDataLoss);
}

TEST(LegacyEnvelopeTest, EnforcesSourceAndSummaryCeilingsBeforeDecode) {
    std::vector<std::uint8_t> oversized(kLegacySourceByteLimit + 1);
    EXPECT_EQ(Import(oversized).status().code(),
              absl::StatusCode::kResourceExhausted);

    std::vector<std::uint8_t> summary(kLegacySummaryByteLimit + 8, 's');
    std::copy_n(
        std::array<std::uint8_t, 7>{0xEF, 0xBB, 0xBF, 'Y', 'S', 'G', 'P'}
            .begin(),
        7, summary.begin());
    ExpectStatus(summary, absl::StatusCode::kResourceExhausted);
}

TEST(LegacyEnvelopeTest, DecodesHistoricalDynamicTransformVectorConcurrently) {
    const auto& directory = test::FixtureDirectory();
    if (directory.empty()) GTEST_SKIP() << "Supply --fixture-dir=<path>";

    const auto fixture = directory / "legacy_v3_dynamic_vector.ysm";
    SCOPED_TRACE(PathToU8(fixture));
    BufferManaged source;
    const auto status = FileRead(fixture, source);
    ASSERT_TRUE(status.ok()) << status;
    std::array<std::future<absl::StatusOr<std::vector<Byte>>>, 4> decodes;
    std::latch start(decodes.size());
    for (auto& pending : decodes) {
        pending = std::async(std::launch::async, [&source, &start]()
                                -> absl::StatusOr<std::vector<Byte>> {
            start.arrive_and_wait();
            ContainerReader reader(source);
            if (auto status = reader.Initialize(); !status.ok()) {
                return status;
            }
            std::vector<Byte> plaintext(12'004);
            if (!reader.ReadExact(plaintext) || !reader.Finish()) {
                return reader.status();
            }
            return plaintext;
        });
    }
    for (auto& pending : decodes) {
        const auto plaintext = pending.get();
        ASSERT_TRUE(plaintext.ok()) << plaintext.status();
        EXPECT_EQ((*plaintext)[0], 1);
        EXPECT_EQ((*plaintext)[1], 0);
        EXPECT_EQ((*plaintext)[2], 0);
        EXPECT_EQ((*plaintext)[3], 0);
        std::mt19937_64 generator(0xD15EA5E);
        for (std::size_t index = 4; index < plaintext->size(); ++index) {
            ASSERT_EQ((*plaintext)[index], static_cast<Byte>(generator()))
                << "offset=" << index;
        }
    }
}
}  // namespace
}  // namespace ysm::legacy::v3::codec
