#include "file_stream.h"

#include <absl/log/absl_log.h>
#include "string_cvt.h"

namespace ysm::codec::sevenzip {
SRes FileStream::Look(const void** buf, size_t* size) noexcept {
    if (buffer_.size() != 0) {
        *buf = buffer_.data() + buffer_position_;
        *size = buffer_.size() - buffer_position_;
        return SZ_OK;
    }

    if (stream_.eof()) [[unlikely]] {
        *size = 0;
        return SZ_ERROR_INPUT_EOF;
    }
    if (stream_.bad()) [[unlikely]] {
        *size = 0;
        return SZ_ERROR_FAIL;
    }

    if (buffer_.size() == 0) [[unlikely]] {
        buffer_.resize(kBufferSize);
    }
    auto offset = stream_.tellg();
    stream_.read(reinterpret_cast<char*>(buffer_.data()),
                 static_cast<std::streamsize>(buffer_.size()));
    if (trace_ && stream_.gcount() > 0)
        trace_->reads.push_back({static_cast<uint64_t>(offset),
                                 static_cast<uint64_t>(stream_.gcount())});
    if (stream_.bad()) [[unlikely]] {
        *size = 0;
        return SZ_ERROR_READ;
    }
    if (stream_.fail()) {
        stream_.clear(stream_.eof() ? std::ios::eofbit : std::ios::goodbit);
    }
    *buf = buffer_.data();
    *size = stream_.gcount();
    return SZ_OK;
}

SRes FileStream::Skip(size_t offset) noexcept {
    // 越界检查
    buffer_position_ += offset;
    if (buffer_position_ > buffer_.size()) [[unlikely]] {
        buffer_position_ -= offset;
        return SZ_ERROR_MEM;
    }
    if (buffer_position_ == buffer_.size()) {
        buffer_position_ = 0;
        buffer_.resize(0);
    }
    return SZ_OK;
}

SRes FileStream::Read(void* buf, size_t* size) noexcept {
    // IO 错误错误
    if (stream_.bad()) [[unlikely]] {
        *size = 0;
        return SZ_ERROR_FAIL;
    }
    // 流已末尾且缓存为空
    if (stream_.eof() && buffer_.size() == 0) [[unlikely]] {
        *size = 0;
        return SZ_ERROR_INPUT_EOF;
    }
    // 空读
    if (*size == 0) {
        return SZ_OK;
    }

    // 读取缓存
    auto consumed = std::min(buffer_.size() - buffer_position_, *size);
    if (consumed > 0) {
        std::memcpy(buf, buffer_.data() + buffer_position_, consumed);
        buffer_position_ += consumed;
        // 缓存读完重置
        if (buffer_position_ == buffer_.size()) {
            buffer_position_ = 0;
            buffer_.resize(0);
        }
        // buf 填充完毕
        if (consumed == *size) {
            return SZ_OK;
        }
        // 未填充完毕但流已末尾
        if (stream_.eof()) {
            *size = consumed;
            return SZ_OK;
        }
    }

    // 缓存不足以填充完 buf，执行读取
    auto remaining = *size - consumed;
    auto offset = stream_.tellg();
    stream_.read(static_cast<char*>(buf) + consumed,
                 static_cast<std::streamsize>(remaining));
    if (trace_ && stream_.gcount() > 0)
        trace_->reads.push_back({static_cast<uint64_t>(offset),
                                 static_cast<uint64_t>(stream_.gcount())});
    // IO 错误
    if (stream_.bad()) [[unlikely]] {
        *size = 0;
        return SZ_ERROR_FAIL;
    }
    if (stream_.fail()) {
        stream_.clear(stream_.eof() ? std::ios::eofbit : std::ios::goodbit);
    }
    // 读出的数据大小不足以填充 buf
    if (remaining != stream_.gcount()) {
        *size = consumed + stream_.gcount();
    }
    return SZ_OK;
}

SRes FileStream::Seek(Int64* pos, ESzSeek origin) noexcept {
    buffer_.resize(0);
    buffer_position_ = 0;
    if (stream_.fail()) [[unlikely]] {
        stream_.clear();
    }
    switch (origin) {
        case SZ_SEEK_SET:
            stream_.seekg(*pos, std::ios::beg);
            break;
        case SZ_SEEK_CUR:
            stream_.seekg(*pos, std::ios::cur);
            break;
        case SZ_SEEK_END:
            stream_.seekg(*pos, std::ios::end);
            break;
        default:
            return SZ_ERROR_PARAM;
    }
    if (stream_.bad()) [[unlikely]] {
        return SZ_ERROR_FAIL;
    }
    if (stream_.fail()) [[unlikely]] {
        stream_.clear(stream_.eof() ? std::ios::eofbit : std::ios::goodbit);
        return SZ_ERROR_FAIL;
    }
    stream_.clear();
    *pos = static_cast<Int64>(stream_.tellg());
    return SZ_OK;
}

FileStream::FileStream(const fs::path& path, ArchiveTrace* trace)
    : trace_(trace),
      vtable_holder_(
          this,
          {
              [](ILookInStreamPtr p, const void** buf, size_t* size) {
                  return Z7_CONTAINER_FROM_VTBL(p, decltype(vtable_holder_), vt)
                      ->ctx->Look(buf, size);
              },
              [](ILookInStreamPtr p, size_t offset) {
                  return Z7_CONTAINER_FROM_VTBL(p, decltype(vtable_holder_), vt)
                      ->ctx->Skip(offset);
              },
              [](ILookInStreamPtr p, void* buf, size_t* size) {
                  return Z7_CONTAINER_FROM_VTBL(p, decltype(vtable_holder_), vt)
                      ->ctx->Read(buf, size);
              },
              [](ILookInStreamPtr p, Int64* pos, ESzSeek origin) {
                  return Z7_CONTAINER_FROM_VTBL(p, decltype(vtable_holder_), vt)
                      ->ctx->Seek(pos, origin);
              },
          }) {

    stream_.exceptions(std::ios::goodbit);
    stream_.open(path, std::ios::in | std::ios::binary);
}

absl::Status FileStream::Ok() const noexcept {
    YSM_ASSERT(stream_.is_open() && stream_.good(),
               absl::UnavailableError("Failed to open file"sv));
    return OkStatus();
}

[[nodiscard]] ILookInStreamPtr FileStream::vtable() const noexcept {
    return &vtable_holder_.vt;
}

FileStream::~FileStream() noexcept {
    if (stream_.is_open()) {
        stream_.close();
    }
}
}  // namespace ysm::codec::sevenzip
