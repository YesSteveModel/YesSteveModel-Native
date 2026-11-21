#include <storage/cache_remote.h>

#include "cache_queries.h"

#include <array>
#include <stdexcept>

namespace ysm::storage {

using detail::BindText;
using detail::CheckCacheTables;
using detail::CheckKey;
using detail::CheckTable;
using detail::Connection;
using detail::EventTime;
using detail::Layout;
using detail::LookupCacheFile;
using detail::OpenState;
using detail::PublishCacheFile;
using detail::ReadReferences;
using detail::Success;

namespace {

void CheckRemoteTables(SQLite::Database& db) {
    CheckCacheTables(db);
    CheckTable(db, "content_refs"_sv, {{"source"_sv, "TEXT"_sv, 1, 1}, {"model"_sv, "TEXT"_sv, 1, 0}, {"container"_sv, "TEXT"_sv, 1, 0}});
    CheckTable(db, "usage"_sv, {{"container"_sv, "TEXT"_sv, 1, 1}, {"seconds"_sv, "INTEGER"_sv, 1, 0}});
}

constexpr Layout kLayout{
    .filename = "remote.sqlite"_sv,
    .role = 10,
    .epoch = 1,
    .revision = 1,
    .journal_mode = "wal"_sv,
    .schema = "CREATE TABLE exact(kind INTEGER NOT NULL,key TEXT NOT NULL,file_id INTEGER NOT NULL,"
            "instance TEXT NOT NULL,value BLOB NOT NULL,PRIMARY KEY(kind,key)) STRICT;"
            "CREATE TABLE content_refs(source TEXT PRIMARY KEY,model TEXT NOT NULL,container TEXT NOT NULL) STRICT;"
            "CREATE INDEX content_sources ON content_refs(container);"
            "CREATE TABLE coverage(id INTEGER PRIMARY KEY CHECK(id=1),complete INTEGER NOT NULL CHECK(complete IN(0,1))) STRICT;"
            "INSERT INTO coverage VALUES(1,0);"
            "CREATE TABLE usage(container TEXT PRIMARY KEY,seconds INTEGER NOT NULL) STRICT;"_sv,
    .validate_tables = CheckRemoteTables,
};

void CheckRemoteKind(ArtifactKind kind) {
    if (kind != ArtifactKind::kMetadata && kind != ArtifactKind::kChunk && kind != ArtifactKind::kPreview)
        throw std::invalid_argument("Artifact does not belong to remote cache storage");
}
Result MaxCacheUsage(Connection& connection, std::string_view container, std::int64_t utc_seconds) {
    CheckKey(container);
    if (utc_seconds < 0) throw std::invalid_argument("Negative usage event time");
    auto& query = connection.Query("INSERT INTO usage(container,seconds) VALUES(?,?) ON CONFLICT(container) "
        "DO UPDATE SET seconds=max(seconds,excluded.seconds)"_sv);
    BindText(query, 1, container); query.bind(2, utc_seconds); query.exec();
    return Success();
}
Values<std::int64_t> ReadCacheUsage(Connection& connection, std::string_view container) {
    CheckKey(container);
    auto& query = connection.Query("SELECT seconds FROM usage WHERE container=?"_sv); BindText(query, 1, container);
    if (!query.executeStep()) return {.result = Success(Outcome::kMissing)};
    return {.result = Success(Outcome::kFound), .values = {EventTime(query, 0)}};
}

}  // namespace

class CacheRemote::Impl { public: std::unique_ptr<Connection> connection; };
YSM_PIMPL_DEFINITION(CacheRemote)
CacheRemote::CacheRemote() = default;
CacheRemote::~CacheRemote() = default;
std::pair<std::unique_ptr<CacheRemote>, Result> CacheRemote::Open(
    const std::filesystem::path& root, const CancellationState& cancelled, bool existing_only) {
    auto owner = std::unique_ptr<CacheRemote>(new CacheRemote());
    auto [connection, result] = OpenState(root, kLayout, cancelled, false, existing_only);
    if (!connection) return {nullptr, result};
    owner->Pimpl().connection = std::move(connection);

    return {std::move(owner), result};
}

Values<CacheFile> CacheRemote::LookupExact(ArtifactKind kind, std::string_view key, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckRemoteKind(kind); return LookupCacheFile(connection, kind, key);
    });
}
Result CacheRemote::PublishExact(ArtifactKind kind, std::string_view key, const CacheFile& file,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckRemoteKind(kind); return PublishCacheFile(connection, kind, key, file);
    });
}
Result CacheRemote::PublishMetadata(std::string_view source, const CacheFile& file, std::string_view model,
    std::string_view container, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(source); CheckKey(model); CheckKey(container);
        connection.Exec("BEGIN IMMEDIATE"_sv);
        PublishCacheFile(connection, ArtifactKind::kMetadata, source, file);
        auto& query = connection.Query("INSERT INTO content_refs(source,model,container) VALUES(?,?,?) "
            "ON CONFLICT(source) DO UPDATE SET model=excluded.model,container=excluded.container"_sv);
        BindText(query, 1, source); BindText(query, 2, model); BindText(query, 3, container); query.exec();
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Result CacheRemote::SetReference(std::string_view source, std::string_view model, std::string_view container,
    bool present, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(source); CheckKey(model); CheckKey(container);
        auto& query = connection.Query(present
            ? "INSERT INTO content_refs(source,model,container) VALUES(?,?,?) ON CONFLICT(source) DO UPDATE SET model=excluded.model,container=excluded.container"_sv
            : "DELETE FROM content_refs WHERE source=? AND model=? AND container=?"_sv);
        BindText(query, 1, source); BindText(query, 2, model); BindText(query, 3, container); query.exec(); return Success();
    });
}
Result CacheRemote::SetReferencesComplete(bool complete, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        auto& query = connection.Query("UPDATE coverage SET complete=? WHERE id=1"_sv);
        query.bind(1, static_cast<int>(complete)); query.exec(); return Success();
    });
}
Values<ContentReferences> CacheRemote::References(std::string_view container,
    const CancellationState& cancelled) {
    return ReferencesMany(std::array{std::string(container)}, cancelled);
}
Values<ContentReferences> CacheRemote::ReferencesMany(std::span<const std::string> containers,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        return ReadReferences(connection, containers, cancelled, "SELECT complete,1,0 FROM coverage WHERE id=1"_sv,
            "SELECT EXISTS(SELECT 1 FROM content_refs WHERE container=?)"_sv);
    });
}
Result CacheRemote::MaxUsage(std::string_view container, std::int64_t utc_seconds, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) { return MaxCacheUsage(connection, container, utc_seconds); });
}
Values<std::int64_t> CacheRemote::ReadUsage(std::string_view container, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) { return ReadCacheUsage(connection, container); });
}

}  // namespace ysm::storage
