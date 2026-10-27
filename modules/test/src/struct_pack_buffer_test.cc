#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <iguana/detail/string_resize.hpp>
#include <ylt/struct_pack.hpp>

#include "buffer_managed.h"

namespace ysm {
namespace {
TEST(StructPackBufferTest, ManagedOutputMatchesStandardBuffers) {
    const std::string text("\0payload\xff", 9);
    const std::array<uint32_t, 3> values{0, 0x12345678, UINT32_MAX};
    const auto expected = struct_pack::serialize(text, values);
    const auto string_output = struct_pack::serialize<std::string>(text, values);

    BufferManaged output;
    constexpr size_t kPrefixSize = 7;
    struct_pack::serialize_to_with_offset(output, kPrefixSize, text, values);
    ASSERT_EQ(output.size(), kPrefixSize + expected.size());
    EXPECT_EQ(std::string_view(
                  reinterpret_cast<const char*>(output.data() + kPrefixSize),
                  expected.size()),
              std::string_view(expected.data(), expected.size()));
    EXPECT_EQ(string_output, std::string(expected.data(), expected.size()));

    std::string decoded_text;
    std::array<uint32_t, 3> decoded_values{};
    ASSERT_FALSE(struct_pack::deserialize_to(
        decoded_text,
        reinterpret_cast<const char*>(output.data() + kPrefixSize),
        expected.size(),
        decoded_values));
    EXPECT_EQ(decoded_text, text);
    EXPECT_EQ(decoded_values, values);
}

TEST(StructPackBufferTest, VectorResizePreservesElementCountAndInitialization) {
    std::vector<uint32_t> packed_values{11, 22};
    std::vector<uint32_t> protobuf_values = packed_values;
    struct_pack::detail::resize(packed_values, 5);
    iguana::detail::resize(protobuf_values, 5);
    const std::vector<uint32_t> expected{11, 22, 0, 0, 0};
    EXPECT_EQ(packed_values, expected);
    EXPECT_EQ(protobuf_values, expected);

    struct_pack::detail::resize(packed_values, 1);
    iguana::detail::resize(protobuf_values, 1);
    EXPECT_EQ(packed_values, (std::vector<uint32_t>{11}));
    EXPECT_EQ(protobuf_values, packed_values);
}
}  // namespace
}  // namespace ysm
