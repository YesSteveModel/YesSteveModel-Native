#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <cancellation.h>
#include <pimpl.h>
#include <storage/cache_types.h>
#include <storage/result.h>

namespace ysm::storage {

enum class BakeKind : std::int32_t { kGeometry, kAnimation };
struct BakePartition {
    BakeKind kind = BakeKind::kGeometry;
    std::string cache_version;
    std::string container;
    std::string binding;
    std::string purpose;
};
struct BakeKey {
    BakePartition partition;
    std::string exact;
    std::string abi;
    std::string capability;
    std::string engine_package;
    std::string compilation;
    std::string frozen;
};
struct BakeObject {
    CacheFile file;
    BakeKey key;
};

// Roots below are the new CACHE/f directory, already created and guarded by
// the caller. The Java owner serializes complete operations and close.
class BakeIndex final {
public:
    // Read-only mode never creates a missing database, schema, WAL or partition.
    static std::pair<std::unique_ptr<BakeIndex>, Result> Open(
        const std::filesystem::path& root, bool read_only, const CancellationState& cancelled);
    ~BakeIndex();
    Values<CacheFile> LookupExact(const BakeKey& key, const CancellationState& cancelled);
    Values<bool> HasPartition(const BakePartition& partition, const CancellationState& cancelled);
    Values<bool> OwnsFile(std::uint64_t file_id, const CancellationState& cancelled);
    Result Publish(const BakeObject& object, const CancellationState& cancelled);
    Values<BakeObject> PageObjects(std::optional<std::uint64_t> after_id, const CancellationState& cancelled);
    Result Revoke(const CacheLocation& location, const CancellationState& cancelled);
    Result Forget(const CacheLocation& location, const CancellationState& cancelled);
    BakeIndex(const BakeIndex&) = delete;
    BakeIndex& operator=(const BakeIndex&) = delete;
private:
    BakeIndex();
    YSM_PIMPL_DECLARE(Impl, 2 * sizeof(void*))
};

}  // namespace ysm::storage
