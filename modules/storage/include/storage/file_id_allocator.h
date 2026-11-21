#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

#include <cancellation.h>
#include <pimpl.h>
#include <storage/result.h>

namespace ysm::storage {

// Roots below are the new CACHE/f directory, already created and guarded by
// the caller. The Java owner serializes complete operations and close.
class FileIdAllocator final {
public:
    static std::pair<std::unique_ptr<FileIdAllocator>, Result> Open(
        const std::filesystem::path& root, const CancellationState& cancelled);
    ~FileIdAllocator();
    Values<std::uint64_t> Allocate(const CancellationState& cancelled);
    FileIdAllocator(const FileIdAllocator&) = delete;
    FileIdAllocator& operator=(const FileIdAllocator&) = delete;
private:
    FileIdAllocator();
    YSM_PIMPL_DECLARE(Impl, sizeof(void*))
};

}  // namespace ysm::storage
