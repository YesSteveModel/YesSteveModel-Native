#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <cancellation.h>
#include <pimpl.h>
#include <storage/cache_types.h>
#include <storage/result.h>

namespace ysm::storage {

struct ConvertedObject {
    CacheLocation location;
    std::string scope;
    std::string cache_version;
    std::string model;
    std::string container;
    bool reusable = true;
};
struct CacheSource {
    std::string source;
    std::string model;
    std::string container;
    std::optional<ConvertedObject> converted;
    std::string observation;
    std::string metadata;
};
struct SourceReference {
    std::string_view source;
    std::string_view model;
    std::string_view container;
};
struct CacheScope {
    std::string scope;
    bool complete = false;
};
struct CacheMaintenance {
    std::optional<std::int64_t> last_success_utc;
    std::uint64_t reset_revision = 0;
    bool pending_all = false;
};

// Roots below are the new CACHE/f directory, already created and guarded by
// the caller. The Java owner serializes complete operations and close.
class CacheCatalog final {
public:
    // Maintenance requires existing source authority; absence must not initialize an empty replacement.
    static std::pair<std::unique_ptr<CacheCatalog>, Result> Open(
        const std::filesystem::path& root, const CancellationState& cancelled, bool existing_only = false);
    ~CacheCatalog();
    Values<CacheFile> LookupExact(ArtifactKind kind, std::string_view key, const CancellationState& cancelled);
    Result PublishExact(ArtifactKind kind, std::string_view key, const CacheFile& file,
        const CancellationState& cancelled);
    Values<std::string> LookupDirectory(std::string_view key, const CancellationState& cancelled);
    Result PublishDirectory(std::string_view key, std::string_view metadata, const CancellationState& cancelled);
    Values<CacheSource> LookupSource(std::string_view scope, std::string_view source,
        const CancellationState& cancelled);
    Values<CacheSource> PageSources(std::string_view scope, std::string_view after_source,
        const CancellationState& cancelled);
    Result PublishSource(std::string_view scope, const CacheSource& source, const CancellationState& cancelled);
    // A nonempty batch of at most kMaximumRows commits atomically. Replaced sources lose their converted links.
    Result PublishSourceReferences(std::string_view scope, std::span<const SourceReference> sources,
        const CancellationState& cancelled);
    // Incomplete domains prevent absence from being treated as proof. Scope
    // records persist when unscanned; callers cannot infer that they vanished.
    Result SetScopeComplete(std::string_view scope, bool complete, const CancellationState& cancelled);
    Result SetReferencesComplete(bool complete, const CancellationState& cancelled);
    Values<CacheScope> PageScopes(std::string_view after_scope, const CancellationState& cancelled);
    Result FinishScan(std::string_view scope, std::span<const std::string> obsolete_sources,
        const CancellationState& cancelled);
    Values<ContentReferences> References(std::string_view container,
        const CancellationState& cancelled);
    // One snapshot, in input order. A failed batch has no reference rows.
    Values<ContentReferences> ReferencesMany(std::span<const std::string> containers,
        const CancellationState& cancelled);
    Values<ConvertedObject> PageConverted(std::optional<std::uint64_t> after_id,
        const CancellationState& cancelled);
    Values<bool> OwnsFile(std::uint64_t file_id, const CancellationState& cancelled);
    Values<ConvertedObject> PageConvertedScope(std::string_view scope, std::optional<std::uint64_t> after_id,
        const CancellationState& cancelled);
    Result RevokeConverted(const CacheLocation& location, const CancellationState& cancelled);
    // Called only after a successful/missing physical deletion. A still linked
    // object or a different production token is rejected.
    Result ForgetConverted(const CacheLocation& location, const CancellationState& cancelled);
    Values<CacheMaintenance> ReadMaintenance(const CancellationState& cancelled);
    Result ResetMaintenance(bool request_all, const CancellationState& cancelled);
    Result CompleteMaintenance(std::uint64_t accepted_revision, std::int64_t utc_seconds,
        const CancellationState& cancelled);
    CacheCatalog(const CacheCatalog&) = delete;
    CacheCatalog& operator=(const CacheCatalog&) = delete;
private:
    CacheCatalog();
    YSM_PIMPL_DECLARE(Impl, sizeof(void*))
};

}  // namespace ysm::storage
