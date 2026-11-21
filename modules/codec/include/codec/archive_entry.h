#pragma once

#include <optional>
#include "c_string_view.h"

namespace ysm::codec {
class ArchiveEntry {
    std::string full_name_;
    std::pair<uint16_t, uint16_t> file_name_range_;
    size_t size_;
    std::optional<uint32_t> crc_;

   protected:
    ArchiveEntry(std::string&& full_name, size_t size,
                 std::optional<uint32_t> crc = std::nullopt)
        : full_name_(std::move(full_name)),
          file_name_range_(GetFileNameRange()),
          size_(size),
          crc_(crc) {}

   public:
    ArchiveEntry(const ArchiveEntry& other) = delete;
    ArchiveEntry& operator=(const ArchiveEntry&) = delete;

    ArchiveEntry(ArchiveEntry&& other) noexcept = default;
    ArchiveEntry& operator=(ArchiveEntry&&) noexcept = default;

    [[nodiscard]] CStringView FullName() const noexcept { return full_name_; }

    [[nodiscard]] CStringView FileName() const noexcept {
        return {full_name_.data() + file_name_range_.first,
                file_name_range_.second};
    }

    [[nodiscard]] size_t Size() const noexcept { return size_; }

    [[nodiscard]] std::optional<uint32_t> Crc() const noexcept { return crc_; }

   private:
    std::pair<uint16_t, uint16_t> GetFileNameRange() const {
        auto slash_pos = full_name_.find_last_of('/');
        if (slash_pos == std::string::npos) {
            return {0, full_name_.size()};
        }
        if (slash_pos == full_name_.size() - 1) {
            return {0, 0};
        }

        auto offset = slash_pos + 1;
        return {offset, full_name_.size() - offset};
    }
};
}  // namespace ysm::codec
