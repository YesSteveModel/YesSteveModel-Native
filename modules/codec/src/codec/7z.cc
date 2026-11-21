#include "codec/7z.h"

#include <7z.h>
#include <7zCrc.h>
#include <7zTypes.h>
#include <mutex>
#include <variant>

#include "7z/file_stream.h"
#include "7z/mem_stream.h"
#include "err.h"
#include "non_copyable.h"
#include "string_cvt.h"

namespace ysm::codec {
using namespace sevenzip;

namespace {
void CheckInit() {
    [[maybe_unused]] static struct Init {
        Init() noexcept { CrcGenerateTable(); }
    } init;
}

constexpr ISzAlloc kAllocator{[](ISzAllocPtr, size_t size) noexcept -> void* {
                                  return new (std::nothrow) std::byte[size];
                              },
                              [](ISzAllocPtr, void* address) noexcept {
                                  delete[] static_cast<std::byte*>(address);
                              }};

struct Catalog : NonCopyable {
    CSzArEx db{};
    std::vector<std::optional<BufferManaged>> blocks;

    Catalog() noexcept { SzArEx_Init(&db); }

    ~Catalog() noexcept { SzArEx_Free(&db, &kAllocator); }
};

}  // namespace

class SevenZip::Impl {
    std::variant<FileStream, MemoryStream> stream_;
    ILookInStreamPtr vtable_;
    mutable Catalog catalog_;
    bool ok_ = false;
    ArchiveTrace* trace_ = nullptr;

   public:
    explicit Impl(fs::path path, ArchiveTrace* trace)
        : stream_(std::in_place_type<FileStream>, std::move(path), trace),
          vtable_(std::get<FileStream>(stream_).vtable()),
          trace_(trace) {
        Open();
    }

    explicit Impl(BufferViewR data)
        : stream_(std::in_place_type<MemoryStream>, data),
          vtable_(std::get<MemoryStream>(stream_).vtable()) {
        Open();
    }

    Impl& operator=(const Impl& other) = delete;

    absl::Status Ok() const {
        YSM_ASSERT(ok_,
                   absl::FailedPreconditionError("7z archive not open."sv));
        return OkStatus();
    }

    [[nodiscard]] absl::Status VisitEntries(
        absl::FunctionRef<void(Entry&&)> visitor) const {
        YSM_ASSERT(ok_, Ok());
        std::u16string str;
        auto db = &catalog_.db;
        static_assert(sizeof(std::u16string::value_type) == sizeof(UInt16));
        for (UInt32 i = 0; i < db->NumFiles; i++) {
            if (SzArEx_IsDir(db, i)) [[unlikely]]
                continue;

            size_t len = SzArEx_GetFileNameUtf16(db, i, nullptr);
            YSM_ASSERT(len > 1,
                       absl::DataLossError("Invalid 7z member name"sv));

            str.resize(len);
            YSM_ASSERT(SzArEx_GetFileNameUtf16(
                           db, i, reinterpret_cast<UInt16*>(str.data())) == len,
                       absl::DataLossError("Invalid 7z member name"sv));
            str.resize(len - 1);

            YSM_DECLARE_OR_RETURN(file_name, U16ToU8(str));
            auto file_size = SzArEx_GetFileSize(db, i);
            auto crc = SzBitWithVals_Check(&db->CRCs, i)
                           ? std::optional<uint32_t>(db->CRCs.Vals[i])
                           : std::nullopt;
            visitor(Entry(std::move(file_name), file_size, i, crc));
        }
        return OkStatus();
    }

    absl::Status DecompressTo(const Entry& entry, BufferManaged& buffer) const {
        YSM_ASSERT(ok_, Ok());
        if (trace_)
            ++trace_->extracts;
        if (entry.Size() == 0) {
            buffer.resize(0);
            return OkStatus();
        }
        auto& ar = catalog_.db;

        YSM_ASSERT(entry.file_index_ < ar.NumFiles,
                   absl::InvalidArgumentError("File index out of range"sv));
        auto folder_index = ar.FileToFolder[entry.file_index_];
        if (folder_index == std::numeric_limits<UInt32>::max()) [[unlikely]] {
            buffer.resize(0);
            return OkStatus();
        }

        auto& folder = catalog_.blocks.at(folder_index);
        if (!folder) {
            auto& block = folder.emplace();
            auto folder_size = SzAr_GetFolderUnpackSize(&ar.db, folder_index);
            block.resize(folder_size);
            auto res =
                SzAr_DecodeFolder(&ar.db, folder_index, vtable_, ar.dataPos,
                                  block.data(), block.size(), &kAllocator);
            YSM_ASSERT(SZ_OK == res,
                       absl::InternalError("Error extracting entry"sv));
        }
        auto& block = folder.value();

        auto unpack_pos = ar.UnpackPositions[entry.file_index_];
        auto data_offset =
            unpack_pos - ar.UnpackPositions[ar.FolderToFile[folder_index]];
        YSM_ASSERT(data_offset + entry.Size() <= block.size(),
                   absl::DataLossError("Corruption data"sv));
        buffer.resize(entry.Size());
        Copy(Slice(block, data_offset, entry.Size()), buffer);
        return OkStatus();
    }

   private:
    void Open() {
        auto db = &catalog_.db;
        CheckInit();
        if (trace_)
            ++trace_->directories;
        if (SzArEx_Open(db, vtable_, &kAllocator, &kAllocator) != SZ_OK)
            [[unlikely]] {
            return;
        }
        ok_ = true;
        catalog_.blocks.resize(db->db.NumFolders);
    }
};

YSM_PIMPL_DEFINITION(SevenZip)

SevenZip::SevenZip(BufferViewR seven_zip_buffer)
    : YSM_PIMPL_CONSTRUCT(seven_zip_buffer) {}

SevenZip::SevenZip(const fs::path& path, ArchiveTrace* trace)
    : YSM_PIMPL_CONSTRUCT(path, trace) {}

SevenZip::~SevenZip() = default;

absl::Status SevenZip::Ok() const {
    return Pimpl().Ok();
}

absl::Status SevenZip::Visit(absl::FunctionRef<void(Entry&&)> visitor) const {
    return Pimpl().VisitEntries(visitor);
}

absl::Status SevenZip::Extract(const Entry& entry,
                               BufferManaged& buffer) const {
    YSM_RETURN_IF_ERROR(Pimpl().DecompressTo(entry, buffer));
    YSM_ASSERT(
        !entry.Crc() || CrcCalc(buffer.data(), buffer.size()) == *entry.Crc(),
        absl::DataLossError("Invalid 7z member checksum"sv));
    return OkStatus();
}
}  // namespace ysm::codec
