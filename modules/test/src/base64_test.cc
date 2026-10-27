#include <gtest/gtest.h>

#include <cryptopp/base64.h>
#include <cryptopp/filters.h>

#include <string>

#include <codec/base64.h>

namespace ysm::codec {
namespace {
std::string EncodeReference(std::string_view input) {
    std::string encoded;
    CryptoPP::StringSource source(
        std::string(input), true,
        new CryptoPP::Base64Encoder(new CryptoPP::StringSink(encoded), false));
    return encoded;
}

TEST(Base64Test, DecodesAllLengthRemaindersWithoutTruncation) {
    for (size_t size = 0; size <= 192; ++size) {
        SCOPED_TRACE(size);
        std::string expected(size, '\0');
        for (size_t index = 0; index < size; ++index) {
            expected[index] = static_cast<char>(index * 37);
        }
        auto encoded = EncodeReference(expected);
        auto decoded = Base64Decode(StrBuf(encoded));
        ASSERT_TRUE(decoded.ok()) << decoded.status();
        EXPECT_EQ(*decoded, expected);

        while (!encoded.empty() && encoded.back() == '=') {
            encoded.pop_back();
        }
        decoded = Base64Decode(StrBuf(encoded));
        ASSERT_TRUE(decoded.ok()) << decoded.status();
        EXPECT_EQ(*decoded, expected);
    }
}

TEST(Base64Test, PreservesLegacyTextureFileNames) {
    for (const auto name : {"normal_pink_inner.png", "suzuhara_ruru_normal.png"}) {
        auto encoded = EncodeReference(name);
        const auto decoded = Base64Decode(StrBuf(encoded));
        ASSERT_TRUE(decoded.ok()) << decoded.status();
        EXPECT_EQ(*decoded, name);
    }
}

TEST(Base64Test, RejectsInsufficientOutputCapacity) {
    const std::string encoded = "bm9ybWFsX3BpbmtfaW5uZXIucG5n";
    std::string output(20, '\0');
    EXPECT_FALSE(Base64Decode(StrBuf(encoded), StrBuf(output)).ok());
}
}  // namespace
}  // namespace ysm::codec
