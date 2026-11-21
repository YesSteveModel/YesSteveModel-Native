#include "string_cvt.h"

#include <absl/strings/str_cat.h>
#include <simdutf.h>

#include <string>
#include <type_traits>

#if YSM_WINDOWS
#include <windows.h>
#endif

namespace ysm {
absl::StatusOr<std::u16string> U8ToU16(std::string_view input) {
    std::u16string output;
    if (input.empty()) {
        return output;
    }
    if (input.size() > output.max_size()) {
        return absl::ResourceExhaustedError("UTF-16 output is too large"sv);
    }
    output.resize(input.size());
    const auto result = simdutf::convert_utf8_to_utf16_with_errors(
        input.data(), input.size(), output.data());
    if (result.error != simdutf::SUCCESS) {
        return absl::InvalidArgumentError(
            absl::StrCat("Invalid UTF-8 at byte ", result.count));
    }
    output.resize(result.count);
    return output;
}

absl::StatusOr<std::string> U16ToU8(std::u16string_view input) {
    std::string output;
    if (input.empty()) {
        return output;
    }
    // A UTF-16 code unit contributes at most three UTF-8 bytes.
    if (input.size() > output.max_size() / 3) {
        return absl::ResourceExhaustedError("UTF-8 output is too large"sv);
    }
    output.resize(input.size() * 3);
    const auto result = simdutf::convert_utf16_to_utf8_with_errors(
        input.data(), input.size(), output.data());
    if (result.error != simdutf::SUCCESS) {
        return absl::InvalidArgumentError(
            absl::StrCat("Invalid UTF-16 at code unit ", result.count));
    }
    output.resize(result.count);
    return output;
}

#if YSM_WINDOWS
std::string U8ToNative(std::string_view str) {
    if (str.empty()) {
        return {};
    }

    int wide_len = MultiByteToWideChar(
        CP_UTF8,
        0,
        str.data(),
        static_cast<int>(str.size()),
        nullptr,
        0);

    std::wstring wide(wide_len, L'\0');

    MultiByteToWideChar(
        CP_UTF8,
        0,
        str.data(),
        static_cast<int>(str.size()),
        wide.data(),
        wide_len);

    // UTF-16 -> ANSI(ACP)
    int ansi_len = WideCharToMultiByte(
        CP_ACP,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    std::string ansi(ansi_len, '\0');

    WideCharToMultiByte(
        CP_ACP,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        ansi.data(),
        ansi_len,
        nullptr,
        nullptr);

    return ansi;
}

std::string U16ToNative(std::u16string_view str) {
    if (str.empty()) return {};

    int size = WideCharToMultiByte(
        CP_ACP,
        0,
        reinterpret_cast<LPCWCH>(str.data()),
        static_cast<int>(str.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    std::string result(size, '\0');

    WideCharToMultiByte(
        CP_ACP,
        0,
        reinterpret_cast<LPCWCH>(str.data()),
        static_cast<int>(str.size()),
        result.data(),
        size,
        nullptr,
        nullptr);

    return result;
}

std::string NativeToU8(std::string_view str) {
    if (str.empty()) {
        return {};
    }

    // multibyte -> UTF-16
    int wide_len = MultiByteToWideChar(
        CP_ACP,
        0,
        str.data(),
        static_cast<int>(str.size()),
        nullptr,
        0);

    if (wide_len <= 0) {
        return {};
    }

    std::wstring wide(wide_len, L'\0');

    MultiByteToWideChar(
        CP_ACP,
        0,
        str.data(),
        static_cast<int>(str.size()),
        wide.data(),
        wide_len);

    // UTF-16 -> UTF-8
    int utf8_len = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (utf8_len <= 0) {
        return {};
    }

    std::string utf8(utf8_len, '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        utf8_len,
        nullptr,
        nullptr);

    return utf8;
}

std::string PathToU8(const fs::path& path) {
    static_assert(std::is_same_v<fs::path::value_type, wchar_t>);
    const std::wstring_view native(path.c_str());
    if (native.empty()) {
        return {};
    }

    // Native paths retain the Windows replacement behavior for unpaired surrogates.
    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, native.data(), static_cast<int>(native.size()), nullptr,
        0, nullptr, nullptr);
    if (size == 0) {
        return {};
    }
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, native.data(),
                        static_cast<int>(native.size()), output.data(), size,
                        nullptr, nullptr);
    return output;
}
#else
std::string U16ToNative(std::u16string_view str) {
    return U16ToU8(str).value_or(std::string{});
}

std::string U8ToNative(std::string_view str) {
    return {str.data(), str.size()};
}

std::string NativeToU8(std::string_view str) {
    return {str.data(), str.size()};
}

std::string PathToU8(const fs::path& path) {
    static_assert(std::is_same_v<fs::path::value_type, char>);
    return path.native();
}
#endif

fs::path U8ToPath(std::string_view str) {
    return std::u8string_view{reinterpret_cast<const char8_t*>(str.data()),
                              str.size()};
}
}  // namespace ysm
