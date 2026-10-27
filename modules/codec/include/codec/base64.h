#pragma once

#include <string>
#include "buffer.h"
#include "err.h"

namespace ysm::codec {
constexpr size_t Base64EncodeOutputMaxSize(size_t data_size) {
    return data_size * 3 / 2 + 1;
}

constexpr size_t Base64DecodeOutputMaxSize(size_t base64_size) {
    return base64_size / 4 * 3 + base64_size % 4 * 3 / 4;
}

absl::StatusOr<size_t> Base64Encode(BufferViewR input, BufferView output);

absl::StatusOr<size_t> Base64Decode(BufferViewR input, BufferView output);

inline absl::StatusOr<std::string> Base64Encode(BufferViewR input) {
    std::string output(Base64EncodeOutputMaxSize(input.size()), '\0');
    YSM_DECLARE_OR_RETURN(out_size, Base64Encode(input, StrBuf(output)));
    output.resize(out_size);
    return output;
}

inline absl::StatusOr<std::string> Base64Decode(BufferViewR input) {
    std::string output(Base64DecodeOutputMaxSize(input.size()), '\0');
    YSM_DECLARE_OR_RETURN(out_size, Base64Decode(input, StrBuf(output)));
    output.resize(out_size);
    return output;
}
}  // namespace ysm::codec
