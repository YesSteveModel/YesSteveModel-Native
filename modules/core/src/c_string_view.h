#pragma once

// https://github.com/lamarrr/STX/blob/main/include/stx/c_string_view.h

#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <string_view>

namespace ysm {
// guaranteed to be null-terminated
struct CStringView {
    using Iterator = char const*;
    using Pointer = char const*;
    using Size = size_t;
    using Index = size_t;

    static constexpr Size length(char const* c_str) {
        char const* it = c_str;
        while (*it != 0) {
            it++;
        }
        return it - c_str;
    }

    constexpr CStringView() : data_{""}, size_{0} {}

    constexpr CStringView(const std::string& str)
        : data_{str.c_str()}, size_(str.size()) {}

    constexpr CStringView(char const* c_string)
        : data_{c_string}, size_{length(c_string)} {}

    constexpr CStringView(char const* c_string, Size size)
        : data_{c_string}, size_{size} {}

    constexpr char const* c_str() const { return data_; }

    constexpr Pointer data() const { return data_; }

    constexpr Size size() const { return size_; }

    constexpr Iterator begin() const { return data_; }

    constexpr Iterator end() const { return data_ + size_; }

    constexpr bool empty() const { return size_ == 0; }

    char const& operator[](Index index) const { return span()[index]; }

    const char& at(Index index) const { return span()[index]; }

    constexpr bool starts_with(std::string_view other) const {
        if (other.size() > size_) {
            return false;
        }

        return std::memcmp(data_, other.data(), other.size()) == 0;
    }

    constexpr bool starts_with(char c) const {
        return size_ > 0 && data_[0] == c;
    }

    constexpr bool ends_with(std::string_view other) const {
        if (other.size() > size_) {
            return false;
        }

        return std::memcmp(data_ + (size_ - other.size()), other.data(),
                           other.size()) == 0;
    }

    // TODO(lamarrr) ::contains(), add to String as well

    constexpr bool ends_with(char c) const {
        return size_ > 0 && data_[size_ - 1] == c;
    }

    constexpr std::span<const char> span() const { return {data_, size_}; }

    constexpr bool operator==(std::string_view other) const {
        if (size_ != other.size()) {
            return false;
        }

        return std::memcmp(data_, other.data(), size_) == 0;
    }

    constexpr bool operator!=(std::string_view other) const {
        return !(*this == other);
    }

    constexpr operator std::string_view() const {
        return std::string_view{data_, size_};
    }

   private:
    char const* data_ = "";
    Size size_ = 0;
};

constexpr CStringView operator""_sv(char const* str, size_t size) noexcept {
    return {str, size};
}
}  // namespace ysm

namespace std {

template <>
struct formatter<ysm::CStringView, char> : formatter<std::string_view, char> {

    auto format(ysm::CStringView s, format_context& ctx) const {
        return formatter<std::string_view, char>::format(
            std::string_view{s.data(), s.size()}, ctx);
    }
};

}  // namespace std
