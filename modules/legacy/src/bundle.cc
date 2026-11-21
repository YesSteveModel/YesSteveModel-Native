#include "legacy/bundle.h"

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1

#include <cryptopp/aes.h>
#include <cryptopp/base64.h>
#include <cryptopp/filters.h>
#include <cryptopp/md5.h>
#include <cryptopp/modes.h>

#include "codec/base64.h"
#include "codec/java_random.h"
#include "codec/zlib.h"

namespace ysm::legacy::bundle {
namespace {
constexpr BufferFixed<4> kHead{'Y', 'S', 'G', 'P'};

/// 大端序
uint32_t ReadUint32(auto&& input) {
    auto ptr = Consume(input, 4);
    return std::byteswap(*reinterpret_cast<const uint32_t*>(ptr.data()));
}

absl::StatusOr<std::string> ReadString(BufferViewR& input) {
    YSM_ASSERT_BUF_SIZE(input, 4);
    auto size = ReadUint32(input);
    YSM_ASSERT_BUF_SIZE(input, size);
    auto string_buffer = Consume(input, size);
    return std::string{reinterpret_cast<const char*>(string_buffer.data()),
                       string_buffer.size()};
}

uint64_t ToLong(BufferViewR buf) {
    uint64_t value = 0;
    auto ptr = buf.data();
    auto size = buf.size();
    for (auto i = 0; i < size; i++) {
        value = (value << 8) | ptr[i];
    }
    return value;
}

absl::StatusOr<std::string> ReadBase64String(BufferViewR& input) {
    YSM_ASSERT_BUF_SIZE(input, 4);
    auto size = ReadUint32(input);
    YSM_ASSERT_BUF_SIZE(input, size);
    auto base64_buffer = Consume(input, size);
    std::string result(codec::Base64DecodeOutputMaxSize(size), '\0');
    YSM_DECLARE_OR_RETURN(out_size,
                          codec::Base64Decode(base64_buffer, StrBuf(result)));
    result.resize(out_size);
    return result;
}

absl::Status ComputeMd5(BufferViewR file_buffer, BufferFixedView<16> out_key) {
    try {
        CryptoPP::Weak1::MD5 md5;
        auto sink = new CryptoPP::ArraySink(out_key.data(), out_key.size());
        auto hash = new CryptoPP::HashFilter(md5, sink);
        CryptoPP::ArraySource(file_buffer.data(), file_buffer.size(), true,
                              hash);
    } catch (const CryptoPP::Exception&) {
        return DataCorruption();
    }
    return OkStatus();
}

absl::StatusOr<size_t> Decrypt(BufferViewR cipher, BufferFixedViewR<16> key,
                               BufferFixedViewR<16> iv, BufferView result) {
    try {
        CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption decryptor;
        decryptor.SetKeyWithIV(key.data(), key.size(), iv.data(), iv.size());
        auto sink = new CryptoPP::ArraySink(result.data(), result.size());
        auto transformer = new CryptoPP::StreamTransformationFilter(
            decryptor, sink,
            CryptoPP::StreamTransformationFilter::PKCS_PADDING);
        CryptoPP::ArraySource source(cipher.data(), cipher.size(), true,
                                     transformer);
        return sink->TotalPutLength();
    } catch (const CryptoPP::Exception&) {
        return DataCorruption();
    }
}

absl::StatusOr<bool> Validate(BufferViewR input) {
    YSM_ASSERT_BUF_SIZE(input, 24);

    YSM_ASSERT(Cmp(Consume(input, 4), kHead), DataCorruption());

    auto ver = ReadUint32(input);
    YSM_ASSERT(ver == 1 || ver == 2, DataCorruption());

    auto md5 = Consume(input, 16);
    BufferFixed<16> computed_md5;
    YSM_RETURN_IF_ERROR(ComputeMd5(input, computed_md5));
    YSM_ASSERT(Cmp(md5, computed_md5), DataCorruption());

    return ver == 2;
}

absl::Status DecodeFile(BufferViewR input_buffer, size_t file_size,
                        BufferManaged& dst, BufferManaged& tmp) {
    auto key = Consume(input_buffer, 16_B);
    auto iv = Consume(input_buffer, 16_B);
    auto encrypted_file_buffer = Consume(input_buffer, file_size);

    tmp.resize(file_size);
    YSM_ASSIGN_OR_RETURN(file_size,
                         Decrypt(encrypted_file_buffer, key, iv, tmp));
    tmp.resize(file_size);
    YSM_RETURN_IF_ERROR(codec::ZlibDecompress(tmp, dst));

    return OkStatus();
}

absl::Status DecodeFile2(BufferViewR input_buffer, size_t file_size,
                         BufferManaged& dst, BufferManaged& tmp) {
    auto encrypted_password_size = ReadUint32(input_buffer);

    auto encrypted_password = Consume(input_buffer, encrypted_password_size);
    auto iv = Consume(input_buffer, 16_B);
    auto encrypted_file_buffer = Consume(input_buffer, file_size);

    BufferFixed<16> password_key;
    YSM_RETURN_IF_ERROR(ComputeMd5(encrypted_file_buffer, password_key));
    codec::JavaRandom(ToLong(password_key)).NextBytes(password_key);
    BufferManaged decrypted_password_buffer(encrypted_password_size);

    YSM_RETURN_IF_ERROR(Decrypt(encrypted_password, password_key, iv,
                                decrypted_password_buffer));
    BufferViewR decrypted_password = Slice(decrypted_password_buffer, 0, 16_B);

    tmp.resize(file_size);
    YSM_ASSIGN_OR_RETURN(file_size,
                         Decrypt(encrypted_file_buffer,
                                 Consume(decrypted_password, 16_B), iv, tmp));
    tmp.resize(file_size);
    if (dst.size() < file_size) {
        dst.resize(file_size);
    }
    YSM_RETURN_IF_ERROR(codec::ZlibDecompress(tmp, dst));

    return OkStatus();
}
}  // namespace

LegacyBundle::LegacyBundle(const fs::path& path) {
    if (FileRead(path, cache_).ok()) {
        if (auto result = Validate(cache_); result.ok()) {
            buf_ = Slice(cache_, 24);
            ver2_ = result.value();
            return;
        }
    }
    buf_ = {};
    ver2_ = false;
}

LegacyBundle::LegacyBundle(BufferViewR buf) {
    auto result = Validate(buf);
    if (result.ok()) {
        buf_ = Slice(buf, 24);
        ver2_ = result.value();
    } else {
        buf_ = {};
        ver2_ = false;
    }
}

absl::Status LegacyBundle::Visit(
    absl::FunctionRef<void(Entry&&)> visitor) const {
    YSM_ASSERT(!buf_.empty(), Ok());
    auto buf = buf_;

    while (!buf.empty()) {
        YSM_ASSERT_BUF_SIZE(buf, 4);
        std::string file_name;
        size_t segment_size;
        size_t file_size;
        if (ver2_) {
            YSM_ASSIGN_OR_RETURN(file_name, ReadBase64String(buf));
            YSM_ASSERT_BUF_SIZE(buf, 8);
            file_size = ReadUint32(buf);
            YSM_ASSERT_BUF_SIZE(buf, 4);
            auto password_size = ReadUint32(buf.subspan(0));
            segment_size = size_t{password_size} + 4 + 16 + file_size;
        } else {
            YSM_ASSIGN_OR_RETURN(file_name, ReadString(buf));
            YSM_ASSERT_BUF_SIZE(buf, 4);
            file_size = ReadUint32(buf);
            segment_size = 16 + 16 + file_size;
        }
        YSM_ASSERT_BUF_SIZE(buf, segment_size);
        visitor(
            Entry(std::move(file_name), file_size, Consume(buf, segment_size)));
    }
    return OkStatus();
}

absl::Status LegacyBundle::Extract(const Entry& entry,
                                   BufferManaged& dst) const {
    YSM_ASSERT(!buf_.empty(), Ok());
    if (ver2_) [[likely]] {
        return DecodeFile2(entry.buf_, entry.Size(), dst, tmp_buf_);
    } else {
        return DecodeFile(entry.buf_, entry.Size(), dst, tmp_buf_);
    }
}

absl::Status LegacyBundle::Ok() const {
    YSM_ASSERT(!buf_.empty(), DataCorruption());
    return OkStatus();
}
}  // namespace ysm::legacy::bundle
