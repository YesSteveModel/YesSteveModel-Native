#pragma once

#include <7zTypes.h>
#include <filesystem>
#include <fstream>

#include "buffer.h"
#include "codec/archive_trace.h"
#include "err.h"
#include "fs.h"
#include "non_copyable.h"

namespace ysm::codec::sevenzip {
class FileStream : public NonCopyable {
    constexpr static size_t kBufferSize = 256;

    std::ifstream stream_;
    BufferManaged buffer_;
    size_t buffer_position_ = 0;
    ArchiveTrace* trace_;

    struct {
        FileStream* ctx;
        ILookInStream vt;
    } vtable_holder_;

   public:
    explicit FileStream(const fs::path& path, ArchiveTrace* trace = nullptr);
    absl::Status Ok() const noexcept;

    [[nodiscard]] ILookInStreamPtr vtable() const noexcept;

    ~FileStream() noexcept;

   private:
    SRes Look(const void** buf, size_t* size) noexcept;
    SRes Skip(size_t offset) noexcept;
    SRes Read(void* buf, size_t* size) noexcept;
    SRes Seek(Int64* pos, ESzSeek origin) noexcept;
};
}  // namespace ysm::codec::sevenzip
