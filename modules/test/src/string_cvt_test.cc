#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <string_cvt.h>

namespace ysm {
namespace {
using namespace std::string_view_literals;

TEST(StringConversionTest, RoundTripsMixedUnicodeWithEmbeddedNul) {
    std::string utf8(256, 'a');
    utf8.append("\xEF\xBB\xBF" "\0\xC2\xA2\xE4\xB8\xAD\xF0\x9F\x98\x80"sv);
    std::u16string utf16(256, u'a');
    utf16.append(u"\uFEFF\0\u00A2\u4E2D\U0001F600"sv);

    const auto decoded = U8ToU16(utf8);
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, utf16);

    const auto encoded = U16ToU8(utf16);
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    EXPECT_EQ(*encoded, utf8);
}

TEST(StringConversionTest, EmptyInputReturnsAnEmptyString) {
    const auto utf16 = U8ToU16({});
    ASSERT_TRUE(utf16.ok()) << utf16.status();
    EXPECT_TRUE(utf16->empty());

    const auto utf8 = U16ToU8({});
    ASSERT_TRUE(utf8.ok()) << utf8.status();
    EXPECT_TRUE(utf8->empty());
}

TEST(StringConversionTest, RejectsMalformedUnicode) {
    for (const auto input : {"A\x80"sv, "A\xC0\x80"sv, "A\xE2\x82"sv,
                             "A\xED\xA0\x80"sv, "A\xF4\x90\x80\x80"sv}) {
        EXPECT_EQ(U8ToU16(input).status().code(),
                  absl::StatusCode::kInvalidArgument);
    }
    for (const auto input : {u"A\xD800"sv, u"A\xDC00"sv, u"\xD800" u"A"sv}) {
        EXPECT_EQ(U16ToU8(input).status().code(),
                  absl::StatusCode::kInvalidArgument);
    }
}
}  // namespace
}  // namespace ysm
