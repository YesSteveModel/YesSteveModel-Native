#pragma once

#include <absl/functional/function_ref.h>
#include <optional>

#include <memory>
#include "archive_entry.h"
#include "archive_trace.h"
#include "buffer.h"
#include "err.h"
#include "fs.h"

namespace ysm::codec {
class Zip {
    void* zip_handle_;
    void* stream_handle_;
    bool stream_open_ = false;
    bool zip_open_ = false;
    std::optional<std::string> file_;
    struct TraceStream;
    std::unique_ptr<TraceStream> trace_stream_;
    ArchiveTrace* trace_ = nullptr;
    BufferManaged directory_;
    void* directory_stream_ = nullptr;
    int64_t directory_offset_ = 0;
    uint64_t member_count_ = 0;

   public:
    class Entry : public ArchiveEntry {
        uint64_t crc32_;
        int64_t offset_;

        friend class Zip;

        Entry(std::string&& full_name, size_t size, uint64_t crc32,
              int64_t offset)
            : ArchiveEntry(std::move(full_name), size,
                           static_cast<uint32_t>(crc32)),
              crc32_(crc32),
              offset_(offset) {}
    };

    using EntryType = Entry;

    explicit Zip(BufferViewR zip_buffer);

    explicit Zip(const fs::path& path, ArchiveTrace* trace = nullptr);

    Zip(const Zip& other);

    Zip& operator=(const Zip&) = delete;
    Zip(Zip&&) = delete;
    Zip& operator=(Zip&&) = delete;

    absl::Status Ok() const;

    ~Zip();

    absl::Status Visit(absl::FunctionRef<void(Entry&&)> visitor) const;

    absl::Status Extract(const Entry& entry, BufferManaged& buffer) const;

   private:
    void Open();
    bool ReadDirectory(void* stream);
};
}  // namespace ysm::codec
