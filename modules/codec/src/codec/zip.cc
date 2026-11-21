#include "codec/zip.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_strm_os.h>
#include <mz_zip.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>

#include "err.h"
#include "log.h"
#include "string_cvt.h"

namespace ysm::codec {
namespace {
constexpr int64_t kEndSize = 22;
constexpr int64_t kCommentSizeOffset = 20;
constexpr uint32_t kEndSignature = 0x06054b50;
constexpr int64_t kDirectoryTrailerSize = 98;

bool ReadAt(void* stream, int64_t offset, std::span<Byte> output) {
    return mz_stream_seek(stream, offset, MZ_SEEK_SET) == MZ_OK &&
           mz_stream_read(stream, output.data(),
                          static_cast<int32_t>(output.size())) ==
               static_cast<int32_t>(output.size());
}

uint64_t ReadInteger(std::span<const Byte> bytes, size_t offset, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value |= uint64_t{bytes[offset + i]} << (8 * i);
    return value;
}

struct DirectoryLocation {
    int64_t offset;
    int64_t end;
    int64_t bias;
    uint64_t members;
};

std::optional<DirectoryLocation> LocateDirectory(void* stream) {
    if (mz_stream_seek(stream, 0, MZ_SEEK_END) != MZ_OK)
        return std::nullopt;
    const auto size = mz_stream_tell(stream);
    if (size < kEndSize)
        return std::nullopt;
    std::array<Byte, kEndSize> end{};
    auto position = size - kEndSize;
    const auto earliest = std::max<int64_t>(0, position - UINT16_MAX);
    if (!ReadAt(stream, position, end))
        return std::nullopt;
    while (ReadInteger(end, 0, 4) != kEndSignature ||
           ReadInteger(end, kCommentSizeOffset, 2) != size - position - kEndSize) {
        if (position == earliest)
            return std::nullopt;
        // This window already contains the comment lengths of the next 20
        // candidates, so we can read as far as the first that could end at EOF.
        const auto limit = std::min(kCommentSizeOffset, position - earliest);
        int64_t step = 1;
        while (step < limit && ReadInteger(end, kCommentSizeOffset - step, 2) !=
                   size - position - kEndSize + step)
            ++step;
        position -= step;
        std::memmove(end.data() + step, end.data(), end.size() - step);
        if (!ReadAt(stream, position, std::span(end).first(step)))
            return std::nullopt;
    }
    if (ReadInteger(end, 4, 2) != 0 || ReadInteger(end, 6, 2) != 0 ||
        ReadInteger(end, 8, 2) != ReadInteger(end, 10, 2))
        return std::nullopt;
    auto count = ReadInteger(end, 10, 2);
    auto directory_size = ReadInteger(end, 12, 4);
    auto declared_offset = ReadInteger(end, 16, 4);
    auto directory_end = position;
    if (count == UINT16_MAX || directory_size == UINT32_MAX ||
        declared_offset == UINT32_MAX) {
        std::array<Byte, 20> locator{};
        if (position < 20 || !ReadAt(stream, position - 20, locator) ||
            ReadInteger(locator, 0, 4) != 0x07064b50 ||
            ReadInteger(locator, 4, 4) != 0 || ReadInteger(locator, 16, 4) != 1)
            return std::nullopt;
        auto zip64_position = ReadInteger(locator, 8, 8);
        std::array<Byte, 56> zip64{};
        if (position < 76 || zip64_position > uint64_t(position - 76) ||
            !ReadAt(stream, static_cast<int64_t>(zip64_position), zip64) ||
            ReadInteger(zip64, 0, 4) != 0x06064b50 ||
            ReadInteger(zip64, 4, 8) !=
                uint64_t(position - 20) - zip64_position - 12 ||
            ReadInteger(zip64, 16, 4) != 0 || ReadInteger(zip64, 20, 4) != 0 ||
            ReadInteger(zip64, 24, 8) != ReadInteger(zip64, 32, 8))
            return std::nullopt;
        count = ReadInteger(zip64, 32, 8);
        directory_size = ReadInteger(zip64, 40, 8);
        declared_offset = ReadInteger(zip64, 48, 8);
        directory_end = static_cast<int64_t>(zip64_position);
    }
    if (directory_size > uint64_t(directory_end))
        return std::nullopt;
    const auto physical_offset = directory_end - static_cast<int64_t>(directory_size);
    if (declared_offset > uint64_t(physical_offset))
        return std::nullopt;
    return DirectoryLocation{
        physical_offset, directory_end,
        physical_offset - static_cast<int64_t>(declared_offset), count};
}

bool HasDirectorySignature(void* stream, int64_t offset, int64_t end) {
    constexpr int64_t kHeaderSize = 6;
    std::array<Byte, kHeaderSize> header{};
    return offset >= 0 && offset <= end - kHeaderSize &&
           ReadAt(stream, offset, header) &&
           ReadInteger(header, 0, 4) == 0x05054b50 &&
           ReadInteger(header, 4, 2) == uint64_t(end - offset - kHeaderSize);
}
}  // namespace

struct Zip::TraceStream {
    mz_stream stream{};
    void* base;
    ArchiveTrace* trace;
    int64_t bias = 0;
    bool file_reads = true;

    TraceStream(void* source, ArchiveTrace* sink) : base(source), trace(sink) {
        static mz_stream_vtbl table{
            .is_open =
                [](void* p) {
                    return mz_stream_is_open(
                        static_cast<TraceStream*>(p)->base);
                },
            .read =
                [](void* p, void* data, int32_t size) {
                    auto& self = *static_cast<TraceStream*>(p);
                    if (!self.trace || !self.file_reads)
                        return mz_stream_read(self.base, data, size);
                    auto offset = mz_stream_tell(self.base);
                    auto count = mz_stream_read(self.base, data, size);
                    if (count > 0 && offset >= 0)
                        self.trace->reads.push_back(
                            {static_cast<uint64_t>(offset),
                             static_cast<uint64_t>(count)});
                    return count;
                },
            .tell =
                [](void* p) {
                    auto& self = *static_cast<TraceStream*>(p);
                    auto offset = mz_stream_tell(self.base);
                    return offset < 0 ? offset : offset - self.bias;
                },
            .seek =
                [](void* p, int64_t offset, int32_t origin) {
                    auto& self = *static_cast<TraceStream*>(p);
                    if (origin == MZ_SEEK_SET) {
                        if (offset > INT64_MAX - self.bias)
                            return MZ_SEEK_ERROR;
                        offset += self.bias;
                    }
                    return mz_stream_seek(self.base, offset, origin);
                },
            .error =
                [](void* p) {
                    return mz_stream_error(static_cast<TraceStream*>(p)->base);
                },
            .get_prop_int64 =
                [](void* p, int32_t prop, int64_t* value) {
                    return mz_stream_get_prop_int64(
                        static_cast<TraceStream*>(p)->base, prop, value);
                },
            .set_prop_int64 =
                [](void* p, int32_t prop, int64_t value) {
                    return mz_stream_set_prop_int64(
                        static_cast<TraceStream*>(p)->base, prop, value);
                }};
        stream.vtbl = &table;
    }
};

Zip::Zip(BufferViewR zip_buffer)
    : zip_handle_(mz_zip_create()), stream_handle_(mz_stream_mem_create()) {
    mz_stream_mem_set_buffer(stream_handle_,
                             const_cast<Byte*>(zip_buffer.data()),
                             static_cast<int32_t>(zip_buffer.size()));
    Open();
}

Zip::Zip(const fs::path& path, ArchiveTrace* trace)
    : zip_handle_(mz_zip_create()),
      stream_handle_(mz_stream_os_create()),
      file_(PathToU8(path)),
      trace_(trace) {
    trace_stream_ = std::make_unique<TraceStream>(stream_handle_, trace_);
    Open();
}

Zip::Zip(const Zip& other)
    : zip_handle_(mz_zip_create()),
      stream_handle_(other.file_ ? mz_stream_os_create()
                                 : mz_stream_mem_create()),
      file_(other.file_) {
    if (file_)
        trace_stream_ = std::make_unique<TraceStream>(stream_handle_, nullptr);
    if (!file_) {
        const void* buf = nullptr;
        int32_t len = 0;
        if (mz_stream_mem_get_buffer(other.stream_handle_, &buf) != MZ_OK)
            [[unlikely]]
            return;
        mz_stream_mem_get_buffer_length(other.stream_handle_, &len);
        mz_stream_mem_set_buffer(stream_handle_, const_cast<void*>(buf), len);
    }
    Open();
}

void Zip::Open() {
    if (file_) {
        if (mz_stream_os_open(stream_handle_, file_->c_str(),
                              MZ_OPEN_MODE_READ) != MZ_OK) [[unlikely]]
            return;
    } else {
        if (mz_stream_mem_open(stream_handle_, nullptr, MZ_OPEN_MODE_READ) !=
            MZ_OK) [[unlikely]]
            return;
    }
    stream_open_ = true;
    auto stream = trace_stream_ ? static_cast<void*>(&trace_stream_->stream)
                                : stream_handle_;
    if (file_) {
        if (!ReadDirectory(stream))
            return;
        directory_stream_ = mz_stream_mem_create();
        mz_stream_mem_set_buffer(directory_stream_, directory_.data(),
                                 static_cast<int32_t>(directory_.size()));
        if (mz_stream_mem_open(directory_stream_, nullptr, MZ_OPEN_MODE_READ) !=
            MZ_OK)
            return;
        trace_stream_->base = directory_stream_;
        trace_stream_->file_reads = false;
    }
    if (mz_zip_open(zip_handle_, stream, MZ_OPEN_MODE_READ) != MZ_OK)
        [[unlikely]]
        return;
    zip_open_ = true;
    if (file_) {
        // Keep minizip's directory authority while switching payload reads back to the original file.
        mz_zip_set_cd_stream(zip_handle_, 0, directory_stream_);
        trace_stream_->base = stream_handle_;
        trace_stream_->bias = directory_offset_;
        trace_stream_->file_reads = true;
    }
}

bool Zip::ReadDirectory(void* stream) {
    const auto location = LocateDirectory(stream);
    if (!location)
        return false;
    const auto directory_size = location->end - location->offset;
    if (directory_size > INT32_MAX - kDirectoryTrailerSize)
        return false;
    directory_offset_ = location->bias;
    member_count_ = location->members;
    directory_.resize(static_cast<size_t>(directory_size) + kDirectoryTrailerSize);
    if (!ReadAt(stream, location->offset, Slice(directory_, 0, directory_size)))
        return false;
    // Rebase only directory location fields in a private metadata-only ZIP. Minizip still interprets every member.
    auto tail = Slice(directory_, directory_size);
    std::ranges::fill(tail, Byte{0});
    auto put = [&](size_t offset, uint64_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i)
            tail[offset + i] = static_cast<Byte>(value >> (i * 8));
    };
    put(0, 0x06064b50, 4);
    put(4, 44, 8);
    put(12, 45, 2);
    put(14, 45, 2);
    put(24, member_count_, 8);
    put(32, member_count_, 8);
    put(40, directory_size, 8);
    put(56, 0x07064b50, 4);
    put(64, directory_size, 8);
    put(72, 1, 4);
    put(76, kEndSignature, 4);
    put(84, 65535, 2);
    put(86, 65535, 2);
    put(88, directory_size, 4);
    put(92, 0xffffffff, 4);
    return true;
}

Zip::~Zip() {
    if (zip_handle_) [[likely]] {
        if (zip_open_) [[likely]] {
            mz_zip_close(zip_handle_);
            zip_open_ = false;
        }
        mz_zip_delete(&zip_handle_);
        zip_handle_ = nullptr;
    }
    if (directory_stream_) {
        mz_stream_mem_close(directory_stream_);
        mz_stream_mem_delete(&directory_stream_);
    }
    if (stream_handle_) [[likely]] {
        if (stream_open_) [[likely]] {
            if (!file_) {
                mz_stream_mem_close(stream_handle_);
            } else {
                mz_stream_os_close(stream_handle_);
            }
            stream_open_ = false;
        }
        if (!file_) {
            mz_stream_mem_delete(&stream_handle_);
        } else {
            mz_stream_os_delete(&stream_handle_);
        }
        stream_handle_ = nullptr;
    }
}

absl::Status Zip::Visit(absl::FunctionRef<void(Entry&&)> visitor) const {
    YSM_ASSERT(stream_open_ && zip_open_, Ok());
    if (trace_)
        ++trace_->directories;
    uint64_t members = 0;
    auto result = mz_zip_goto_first_entry(zip_handle_);
    while (result == MZ_OK) {
        ++members;
        auto offset = mz_zip_get_entry(zip_handle_);
        mz_zip_file* entry_info;
        YSM_ASSERT(mz_zip_entry_get_info(zip_handle_, &entry_info) == MZ_OK,
                   absl::InternalError("Error obtaining zip entry info"sv));
        YSM_ASSERT(
            mz_zip_entry_is_symlink(zip_handle_) != MZ_OK,
            absl::InvalidArgumentError("Archive links are not supported"sv));
        if (mz_zip_entry_is_dir(zip_handle_) != MZ_OK) [[likely]] {
            visitor(Entry(entry_info->filename, entry_info->uncompressed_size,
                          entry_info->crc, offset));
        }
        result = mz_zip_goto_next_entry(zip_handle_);
    }
    const auto offset = mz_zip_get_entry(zip_handle_);
    // A digital signature ends the directory, but minizip expects another member header.
    if (file_) {
        const auto end = static_cast<int64_t>(directory_.size()) - kDirectoryTrailerSize;
        YSM_ASSERT(members == member_count_, absl::DataLossError("Invalid ZIP member count"sv));
        if ((result == MZ_END_OF_LIST && offset == end) ||
            (result == MZ_FORMAT_ERROR && HasDirectorySignature(directory_stream_, offset, end)))
            return OkStatus();
    } else if (result == MZ_END_OF_LIST) {
        return OkStatus();
    } else if (result == MZ_FORMAT_ERROR) {
        const auto location = LocateDirectory(stream_handle_);
        if (location && members == location->members && offset >= location->offset &&
            HasDirectorySignature(stream_handle_, offset, location->end))
            return OkStatus();
    }
    return absl::DataLossError("Invalid ZIP member directory"sv);
}

absl::Status Zip::Extract(const Entry& entry, BufferManaged& buffer) const {
    YSM_ASSERT(stream_open_ && zip_open_, Ok());
    if (trace_)
        ++trace_->extracts;
    auto handle = zip_handle_;
    YSM_ASSERT(mz_zip_goto_entry(handle, entry.offset_) == MZ_OK,
               absl::NotFoundError("Entry not found"sv));
    YSM_ASSERT(mz_zip_entry_read_open(handle, false, nullptr) == MZ_OK,
               absl::InternalError("Error opening entry"sv));
    buffer.resize(entry.Size());
    auto size = mz_zip_entry_read(handle, buffer.data(),
                                  static_cast<int32_t>(buffer.size()));
    auto closed = mz_zip_entry_read_close(handle, nullptr, nullptr, nullptr);
    YSM_ASSERT(size == entry.Size(), absl::DataLossError("Invalid data"sv));
    YSM_ASSERT(closed == MZ_OK,
               absl::DataLossError("Invalid ZIP member checksum"sv));
    return OkStatus();
}

absl::Status Zip::Ok() const {
    YSM_ASSERT(stream_open_,
               absl::FailedPreconditionError("Zip stream not open."sv));
    YSM_ASSERT(zip_open_, absl::FailedPreconditionError("Zip not open."sv));
    return OkStatus();
}
}  // namespace ysm::codec
