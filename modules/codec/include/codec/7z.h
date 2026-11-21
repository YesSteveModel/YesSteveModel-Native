#pragma once

#include <absl/functional/function_ref.h>
#include <filesystem>
#include <string>

#include "archive_entry.h"
#include "archive_trace.h"
#include "buffer_managed.h"
#include "c_string_view.h"
#include "err.h"
#include "fs.h"
#include "pimpl.h"

namespace ysm::codec {
class SevenZip {
    YSM_PIMPL_DECLARE(Impl, 1024)
   public:
    class Entry : public ArchiveEntry {
        uint32_t file_index_;

        friend class Impl;

        Entry(std::string&& full_name, size_t size, uint32_t file_index,
              std::optional<uint32_t> crc)
            : ArchiveEntry(std::move(full_name), size, crc),
              file_index_(file_index) {}
    };

    using EntryType = Entry;

    explicit SevenZip(BufferViewR zip_buffer);
    explicit SevenZip(const fs::path& path, ArchiveTrace* trace = nullptr);
    ~SevenZip();

    absl::Status Ok() const;

    absl::Status Visit(absl::FunctionRef<void(Entry&&)> visitor) const;
    absl::Status Extract(const Entry& entry, BufferManaged& buffer) const;
};
}  // namespace ysm::codec
