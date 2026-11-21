#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <cancellation.h>
#include <pimpl.h>
#include <storage/cache_types.h>
#include <storage/result.h>

namespace ysm::storage {

// Roots below are the new CACHE/f directory, already created and guarded by
// the caller. The Java owner serializes complete operations and close.
class CacheRemote final {
public:
    // Maintenance requires existing source authority; absence must not initialize an empty replacement.
    static std::pair<std::unique_ptr<CacheRemote>, Result> Open(
        const std::filesystem::path& root, const CancellationState& cancelled, bool existing_only = false);
    ~CacheRemote();
    Values<CacheFile> LookupExact(ArtifactKind kind, std::string_view key, const CancellationState& cancelled);
    Result PublishExact(ArtifactKind kind, std::string_view key, const CacheFile& file,
        const CancellationState& cancelled);
    Result PublishMetadata(std::string_view source, const CacheFile& file, std::string_view model,
        std::string_view container, const CancellationState& cancelled);
    Result SetReference(std::string_view source, std::string_view model, std::string_view container,
        bool present, const CancellationState& cancelled);
    Result SetReferencesComplete(bool complete, const CancellationState& cancelled);
    Values<ContentReferences> References(std::string_view container,
        const CancellationState& cancelled);
    // One snapshot, in input order. A failed batch has no reference rows.
    Values<ContentReferences> ReferencesMany(std::span<const std::string> containers,
        const CancellationState& cancelled);
    Result MaxUsage(std::string_view container, std::int64_t utc_seconds, const CancellationState& cancelled);
    Values<std::int64_t> ReadUsage(std::string_view container, const CancellationState& cancelled);
    CacheRemote(const CacheRemote&) = delete;
    CacheRemote& operator=(const CacheRemote&) = delete;
private:
    CacheRemote();
    YSM_PIMPL_DECLARE(Impl, sizeof(void*))
};

}  // namespace ysm::storage
