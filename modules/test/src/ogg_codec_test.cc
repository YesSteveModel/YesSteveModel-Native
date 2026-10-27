#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <buffer_managed.h>
#include <codec/mini_ogg.h>
#include <codec/ogg_muxer.h>

namespace ysm::codec {
namespace {
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

BufferManaged Write(const OggPagePlan& plan,
                    std::span<const OggMuxPacket> packets) {
    BufferManaged result(plan.size());
    EXPECT_TRUE(WriteOggPage(plan, packets, result).ok());
    return result;
}

TEST(OggPageTest, ValidatesStructureAndCrc) {
    static constexpr std::array<Byte, 3> kPacket{1, 2, 3};
    constexpr std::array packets{
        OggMuxPacket{.bytes = kPacket, .granule_position = 960}};
    const auto plan =
        PlanOggPage(packets, {}, kOggMaxPageSize, 17, 0, true, true);
    ASSERT_TRUE(plan.ok());
    auto page = Write(*plan, packets);

    const auto parsed = ReadOggPage(page);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed->serial_number, 17U);
    EXPECT_EQ(parsed->sequence, 0U);
    EXPECT_EQ(parsed->granule_position, 960U);
    EXPECT_TRUE(parsed->bos);
    EXPECT_TRUE(parsed->eos);
    EXPECT_TRUE(Cmp(parsed->body, BufferViewR(kPacket)));
    EXPECT_EQ(IndependentCrc(page), OggCrc(page));

    page[5] |= 0x08U;
    EXPECT_EQ(ReadOggPage(page).status().code(), absl::StatusCode::kDataLoss);
}

TEST(OggMuxerTest, TerminatesExactMultipleOf255OnContinuationPage) {
    std::vector<Byte> bytes(255 * 255, 0x5a);
    const std::array packets{
        OggMuxPacket{.bytes = bytes, .granule_position = 48'000}};

    const auto first =
        PlanOggPage(packets, {}, kOggMaxPageSize, 9, 0, true, true);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first->segment_count, kOggMaxSegments);
    EXPECT_EQ(first->granule_position, kOggUnknownGranule);
    EXPECT_EQ(first->next.packet_offset, bytes.size());
    EXPECT_FALSE(first->eos);

    const auto second =
        PlanOggPage(packets, first->next, kOggMaxPageSize, 9, 1, false, true);
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second->continuation);
    EXPECT_EQ(second->segment_count, 1U);
    EXPECT_EQ(second->lacing[0], 0);
    EXPECT_EQ(second->granule_position, 48'000U);
    EXPECT_TRUE(second->eos);

    const auto second_page = Write(*second, packets);
    const auto parsed = ReadOggPage(second_page);
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed->continuation);
    EXPECT_TRUE(parsed->eos);
}

TEST(OggMuxerTest, EnforcesActualPageLimit) {
    std::vector<Byte> bytes(100'000, 0x11);
    const std::array packets{
        OggMuxPacket{.bytes = bytes, .granule_position = 1}};
    const auto plan = PlanOggPage(packets, {}, 57'344, 1, 0, true, true);
    ASSERT_TRUE(plan.ok());
    EXPECT_LE(plan->size(), 57'344U);
    EXPECT_FALSE(plan->eos);
    EXPECT_GT(plan->next.packet_offset, 0U);
    EXPECT_LT(plan->next.packet_offset, bytes.size());
}
}  // namespace
}  // namespace ysm::codec
