#pragma once

#include <absl/status/statusor.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "fs.h"

namespace ysm {
std::string U8ToNative(std::string_view str);
std::string U16ToNative(std::u16string_view str);
absl::StatusOr<std::string> U16ToU8(std::u16string_view input);
absl::StatusOr<std::u16string> U8ToU16(std::string_view input);
std::string NativeToU8(std::string_view str);

std::string PathToU8(const fs::path& path);
fs::path U8ToPath(std::string_view str);
}  // namespace ysm
