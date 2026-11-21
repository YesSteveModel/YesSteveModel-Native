#include <storage/cache_catalog.h>

#include "cache_queries.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <sqlite3.h>

namespace ysm::storage {

using detail::BindBlob;
using detail::BindLocation;
using detail::BindText;
using detail::Blob;
using detail::CheckCacheTables;
using detail::CheckInstance;
using detail::CheckKey;
using detail::CheckPayload;
using detail::CheckTable;
using detail::Connection;
using detail::Error;
using detail::EventTime;
using detail::Layout;
using detail::LookupCacheFile;
using detail::OpenState;
using detail::PublishCacheFile;
using detail::ReadLocation;
using detail::ReadObjectPage;
using detail::ReadReferences;
using detail::RequireLayout;
using detail::Scalar;
using detail::SignedId;
using detail::StoredBool;
using detail::Success;
using detail::Text;
using detail::UnsignedId;

namespace {

void CheckCatalogTables(SQLite::Database& db) {
    CheckCacheTables(db);
    CheckTable(db, "directories"_sv, {{"key"_sv, "TEXT"_sv, 1, 1}, {"value"_sv, "BLOB"_sv, 1, 0}});
    CheckTable(db, "scopes"_sv, {{"scope"_sv, "TEXT"_sv, 1, 1}, {"complete"_sv, "INTEGER"_sv, 1, 0}});
    CheckTable(db, "objects"_sv, {{"file_id"_sv, "INTEGER"_sv, 0, 1}, {"instance"_sv, "TEXT"_sv, 1, 0}, {"scope"_sv, "TEXT"_sv, 1, 0},
        {"cache_version"_sv, "TEXT"_sv, 1, 0}, {"model"_sv, "TEXT"_sv, 1, 0}, {"container"_sv, "TEXT"_sv, 1, 0}, {"reusable"_sv, "INTEGER"_sv, 1, 0}});
    CheckTable(db, "sources"_sv, {{"scope"_sv, "TEXT"_sv, 1, 1}, {"source"_sv, "TEXT"_sv, 1, 2}, {"model"_sv, "TEXT"_sv, 1, 0},
        {"container"_sv, "TEXT"_sv, 1, 0}, {"file_id"_sv, "INTEGER"_sv, 0, 0}, {"instance"_sv, "TEXT"_sv, 0, 0},
        {"observation"_sv, "BLOB"_sv, 1, 0}, {"metadata"_sv, "BLOB"_sv, 1, 0}});
    RequireLayout(Scalar(db, "SELECT count(*) FROM pragma_foreign_key_list('sources') WHERE \"table\"='objects'"_sv) == 2,
        "Missing converted object relationship"_sv);
    CheckTable(db, "maintenance"_sv, {{"id"_sv, "INTEGER"_sv, 0, 1}, {"last_success"_sv, "INTEGER"_sv, 0, 0},
        {"reset_revision"_sv, "INTEGER"_sv, 1, 0}, {"pending_all"_sv, "INTEGER"_sv, 1, 0}});
    RequireLayout(Scalar(db, "SELECT count(*) FROM maintenance WHERE id=1 AND pending_all IN(0,1)"_sv) == 1,
        "Missing maintenance state"_sv);
}

constexpr Layout kLayout{
    .filename = "catalog.sqlite"_sv,
    .role = 9,
    .epoch = 1,
    .revision = 1,
    .journal_mode = "wal"_sv,
    .schema = "CREATE TABLE exact(kind INTEGER NOT NULL,key TEXT NOT NULL,file_id INTEGER NOT NULL,"
            "instance TEXT NOT NULL,value BLOB NOT NULL,PRIMARY KEY(kind,key)) STRICT;"
            "CREATE TABLE scopes(scope TEXT PRIMARY KEY,complete INTEGER NOT NULL CHECK(complete IN(0,1))) STRICT;"
            "CREATE TABLE objects(file_id INTEGER PRIMARY KEY,instance TEXT NOT NULL,scope TEXT NOT NULL,"
            "cache_version TEXT NOT NULL,model TEXT NOT NULL,container TEXT NOT NULL,"
            "reusable INTEGER NOT NULL CHECK(reusable IN(0,1)),UNIQUE(file_id,instance)) STRICT;"
            "CREATE TABLE sources(scope TEXT NOT NULL REFERENCES scopes(scope),source TEXT NOT NULL,"
            "model TEXT NOT NULL,container TEXT NOT NULL,file_id INTEGER,instance TEXT,observation BLOB NOT NULL,"
            "metadata BLOB NOT NULL,PRIMARY KEY(scope,source),FOREIGN KEY(file_id,instance) REFERENCES objects(file_id,instance),"
            "CHECK((file_id IS NULL)=(instance IS NULL))) STRICT;"
            "CREATE INDEX content_sources ON sources(container);"
            "CREATE INDEX source_location ON sources(file_id,instance) WHERE file_id IS NOT NULL;"
            "CREATE TABLE directories(key TEXT PRIMARY KEY,value BLOB NOT NULL) STRICT;"
            "CREATE TABLE coverage(id INTEGER PRIMARY KEY CHECK(id=1),complete INTEGER NOT NULL CHECK(complete IN(0,1))) STRICT;"
            "INSERT INTO coverage VALUES(1,0);"
            "CREATE TABLE maintenance(id INTEGER PRIMARY KEY CHECK(id=1),last_success INTEGER,"
            "reset_revision INTEGER NOT NULL,pending_all INTEGER NOT NULL CHECK(pending_all IN(0,1))) STRICT;"
            "INSERT INTO maintenance VALUES(1,NULL,0,0);"_sv,
    .validate_tables = CheckCatalogTables,
};

void CheckCatalogKind(ArtifactKind kind) {
    if (kind != ArtifactKind::kPreview)
        throw std::invalid_argument("Derived or remote artifact does not belong to the cache catalog");
}
ConvertedObject ReadConverted(SQLite::Statement& query, int column = 0) {
    return {ReadLocation(query, column), Text(query, column + 2), Text(query, column + 3),
        Text(query, column + 4), Text(query, column + 5), StoredBool(query, column + 6)};
}
CacheSource ReadCacheSource(SQLite::Statement& query) {
    CacheSource value{Text(query, 0), Text(query, 1), Text(query, 2), std::nullopt, Blob(query, 3), Blob(query, 4)};
    if (value.observation.size() + value.metadata.size() > kMaximumBytes)
        throw SQLite::Exception("Source record exceeds storage bound", SQLITE_CORRUPT);
    if (!query.getColumn(5).isNull()) {
        auto object = ReadConverted(query, 7);
        if (object.location.file_id != UnsignedId(query, 5) || object.location.instance != Text(query, 6) ||
            !object.reusable || object.model != value.model || object.container != value.container)
            throw SQLite::Exception("Broken source object relationship", SQLITE_CORRUPT);
        value.converted = std::move(object);
    } else if (!query.getColumn(6).isNull()) {
        throw SQLite::Exception("Incomplete source object relationship", SQLITE_CORRUPT);
    }
    return value;
}
CacheMaintenance ReadMaintenanceState(Connection& connection) {
    auto& query = connection.Query("SELECT last_success,reset_revision,pending_all FROM maintenance WHERE id=1"_sv);
    if (!query.executeStep()) throw SQLite::Exception("Missing maintenance state", SQLITE_CORRUPT);
    CacheMaintenance state;
    if (!query.getColumn(0).isNull()) state.last_success_utc = EventTime(query, 0);
    state.reset_revision = UnsignedId(query, 1); state.pending_all = StoredBool(query, 2);
    return state;
}

}  // namespace

class CacheCatalog::Impl { public: std::unique_ptr<Connection> connection; };
YSM_PIMPL_DEFINITION(CacheCatalog)
CacheCatalog::CacheCatalog() = default;
CacheCatalog::~CacheCatalog() = default;
std::pair<std::unique_ptr<CacheCatalog>, Result> CacheCatalog::Open(
    const std::filesystem::path& root, const CancellationState& cancelled, bool existing_only) {
    auto owner = std::unique_ptr<CacheCatalog>(new CacheCatalog());
    auto [connection, result] = OpenState(root, kLayout, cancelled, false, existing_only);
    if (!connection) return {nullptr, result};
    owner->Pimpl().connection = std::move(connection);

    return {std::move(owner), result};
}

Values<CacheFile> CacheCatalog::LookupExact(ArtifactKind kind, std::string_view key, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckCatalogKind(kind); return LookupCacheFile(connection, kind, key);
    });
}
Result CacheCatalog::PublishExact(ArtifactKind kind, std::string_view key, const CacheFile& file,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckCatalogKind(kind); return PublishCacheFile(connection, kind, key, file);
    });
}
Values<std::string> CacheCatalog::LookupDirectory(std::string_view key, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<std::string> {
        CheckKey(key);
        auto& query = connection.Query("SELECT value FROM directories WHERE key=?"_sv);
        BindText(query, 1, key);
        if (!query.executeStep()) return {.result = Success(Outcome::kMissing)};
        return {.result = Success(Outcome::kFound), .values = {Blob(query, 0)}};
    });
}
Result CacheCatalog::PublishDirectory(std::string_view key, std::string_view metadata, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(key); CheckPayload(metadata);
        auto& query = connection.Query("INSERT INTO directories(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"_sv);
        BindText(query, 1, key); BindBlob(query, 2, metadata); query.exec(); return Success();
    });
}
Values<CacheSource> CacheCatalog::LookupSource(std::string_view scope, std::string_view source,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<CacheSource> {
        CheckKey(scope); CheckKey(source);
        auto& query = connection.Query("SELECT s.source,s.model,s.container,s.observation,s.metadata,s.file_id,s.instance,"
            "o.file_id,o.instance,o.scope,o.cache_version,o.model,o.container,o.reusable FROM sources s "
            "LEFT JOIN objects o ON s.file_id=o.file_id AND s.instance=o.instance WHERE s.scope=? AND s.source=?"_sv);
        BindText(query, 1, scope); BindText(query, 2, source);
        if (!query.executeStep()) return {.result = Success(Outcome::kMissing)};
        auto value = ReadCacheSource(query);
        if (value.converted && value.converted->scope != scope)
            throw SQLite::Exception("Source scope mismatch", SQLITE_CORRUPT);
        return {.result = Success(Outcome::kFound), .values = {std::move(value)}};
    });
}
Values<CacheSource> CacheCatalog::PageSources(std::string_view scope, std::string_view after_source,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(scope); CheckKey(after_source);
        auto& query = connection.Query("SELECT s.source,s.model,s.container,s.observation,s.metadata,s.file_id,s.instance,"
            "o.file_id,o.instance,o.scope,o.cache_version,o.model,o.container,o.reusable FROM sources s "
            "LEFT JOIN objects o ON s.file_id=o.file_id AND s.instance=o.instance "
            "WHERE s.scope=? AND s.source>? ORDER BY s.source LIMIT 64"_sv);
        BindText(query, 1, scope); BindText(query, 2, after_source);
        Values<CacheSource> output{.result = Success(Outcome::kFound)};
        std::size_t bytes = 0;
        while (query.executeStep()) {
            auto value = ReadCacheSource(query);
            if (value.converted && value.converted->scope != scope)
                throw SQLite::Exception("Source scope mismatch", SQLITE_CORRUPT);
            const auto size = value.observation.size() + value.metadata.size();
            if (bytes + size > kMaximumBytes && !output.values.empty()) break;
            bytes += size; output.values.push_back(std::move(value));
        }
        return output;
    });
}
Result CacheCatalog::PublishSource(std::string_view scope, const CacheSource& source, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(scope); CheckKey(source.source); CheckKey(source.model); CheckKey(source.container);
        CheckPayload(source.observation, source.metadata);
        if (scope.empty() || source.source.empty()) throw std::invalid_argument("Empty source scope or key");
        connection.Exec("BEGIN IMMEDIATE"_sv);
        auto& domain = connection.Query("INSERT INTO scopes(scope,complete) VALUES(?,0) ON CONFLICT(scope) DO NOTHING"_sv);
        BindText(domain, 1, scope); domain.exec();
        if (source.converted) {
            const auto& object = *source.converted;
            CheckKey(object.cache_version); CheckInstance(object.location.instance);
            if (object.scope != scope || object.model != source.model || object.container != source.container || !object.reusable)
                throw std::invalid_argument("Converted source identity mismatch");
            auto& existing = connection.Query("SELECT file_id,instance,scope,cache_version,model,container,reusable FROM objects WHERE file_id=?"_sv);
            existing.bind(1, SignedId(object.location.file_id));
            if (existing.executeStep()) {
                const auto old = ReadConverted(existing);
                if (old.location != object.location || old.scope != scope || old.cache_version != object.cache_version ||
                    old.model != object.model || old.container != object.container || !old.reusable)
                    return Success(Outcome::kRejected);
            } else {
                auto& insert = connection.Query("INSERT INTO objects(file_id,instance,scope,cache_version,model,container,reusable) VALUES(?,?,?,?,?,?,1)"_sv);
                BindLocation(insert, 1, object.location); BindText(insert, 3, scope); BindText(insert, 4, object.cache_version);
                BindText(insert, 5, object.model); BindText(insert, 6, object.container); insert.exec();
            }
        }
        auto& query = connection.Query("INSERT INTO sources(scope,source,model,container,file_id,instance,observation,metadata) VALUES(?,?,?,?,?,?,?,?) "
            "ON CONFLICT(scope,source) DO UPDATE SET model=excluded.model,container=excluded.container,file_id=excluded.file_id,"
            "instance=excluded.instance,observation=excluded.observation,metadata=excluded.metadata"_sv);
        BindText(query, 1, scope); BindText(query, 2, source.source); BindText(query, 3, source.model); BindText(query, 4, source.container);
        if (source.converted) BindLocation(query, 5, source.converted->location);
        else { query.bind(5); query.bind(6); }
        BindBlob(query, 7, source.observation); BindBlob(query, 8, source.metadata); query.exec();
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Result CacheCatalog::PublishSourceReferences(std::string_view scope, std::span<const SourceReference> sources,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(scope);
        if (scope.empty() || sources.empty() || sources.size() > kMaximumRows)
            throw std::invalid_argument("Invalid source reference batch");
        for (const auto& source : sources) {
            CheckKey(source.source); CheckKey(source.model); CheckKey(source.container);
            if (source.source.empty()) throw std::invalid_argument("Empty source key");
        }
        connection.Exec("BEGIN IMMEDIATE"_sv);
        auto& domain = connection.Query("INSERT INTO scopes(scope,complete) VALUES(?,0) ON CONFLICT(scope) DO NOTHING"_sv);
        BindText(domain, 1, scope);
        domain.exec();
        auto& query = connection.Query("INSERT INTO sources(scope,source,model,container,file_id,instance,observation,metadata) "
            "VALUES(?,?,?,?,NULL,NULL,X'',X'') ON CONFLICT(scope,source) DO UPDATE SET model=excluded.model,"
            "container=excluded.container,file_id=NULL,instance=NULL,observation=X'',metadata=X'' "
            "WHERE sources.model<>excluded.model OR sources.container<>excluded.container OR sources.file_id IS NOT NULL "
            "OR sources.instance IS NOT NULL OR sources.observation<>X'' OR sources.metadata<>X''"_sv);
        BindText(query, 1, scope);
        for (const auto& source : sources) {
            if (cancelled.IsRequested()) return Error(Failure::kCancelled, "Operation cancelled"_sv);
            BindText(query, 2, source.source);
            BindText(query, 3, source.model); BindText(query, 4, source.container);
            query.exec();
            query.reset();
        }
        if (cancelled.IsRequested()) return Error(Failure::kCancelled, "Operation cancelled"_sv);
        connection.Exec("COMMIT"_sv);
        return Success();
    });
}
Result CacheCatalog::SetScopeComplete(std::string_view scope, bool complete, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(scope);
        if (scope.empty()) throw std::invalid_argument("Empty source scope");
        auto& query = connection.Query("INSERT INTO scopes(scope,complete) VALUES(?,?) ON CONFLICT(scope) DO UPDATE SET complete=excluded.complete"_sv);
        BindText(query, 1, scope); query.bind(2, static_cast<int>(complete)); query.exec(); return Success();
    });
}
Result CacheCatalog::SetReferencesComplete(bool complete, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        auto& query = connection.Query("UPDATE coverage SET complete=? WHERE id=1"_sv);
        query.bind(1, static_cast<int>(complete)); query.exec(); return Success();
    });
}
Values<CacheScope> CacheCatalog::PageScopes(std::string_view after_scope, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(after_scope);
        auto& query = connection.Query("SELECT scope,complete FROM scopes WHERE scope>? ORDER BY scope LIMIT 64"_sv);
        BindText(query, 1, after_scope);
        Values<CacheScope> output{.result = Success(Outcome::kFound)};
        while (query.executeStep()) output.values.push_back({Text(query, 0), StoredBool(query, 1)});
        return output;
    });
}
Result CacheCatalog::FinishScan(std::string_view scope, std::span<const std::string> obsolete_sources,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(scope);
        if (obsolete_sources.size() >= kMaximumRows) throw std::invalid_argument("Source retirement exceeds bound");
        connection.Exec("BEGIN IMMEDIATE"_sv);
        for (const auto& source : obsolete_sources) {
            CheckKey(source);
            auto& query = connection.Query("DELETE FROM sources WHERE scope=? AND source=?"_sv);
            BindText(query, 1, scope); BindText(query, 2, source); query.exec();
        }
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Values<ContentReferences> CacheCatalog::References(std::string_view container,
    const CancellationState& cancelled) {
    return ReferencesMany(std::array{std::string(container)}, cancelled);
}
Values<ContentReferences> CacheCatalog::ReferencesMany(std::span<const std::string> containers,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        return ReadReferences(connection, containers, cancelled,
            "SELECT complete,1+(SELECT count(*) FROM scopes),(SELECT count(*) FROM scopes WHERE complete=0) "
            "FROM coverage WHERE id=1"_sv,
            "SELECT EXISTS(SELECT 1 FROM sources WHERE container=?)"_sv);
    });
}
Values<ConvertedObject> CacheCatalog::PageConverted(std::optional<std::uint64_t> after_id,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        auto& query = connection.Query("SELECT file_id,instance,scope,cache_version,model,container,reusable FROM objects "
            "WHERE file_id BETWEEN ?1 AND ?2 ORDER BY file_id LIMIT ?3"_sv);
        Values<ConvertedObject> output{.result = Success(Outcome::kFound)};
        ReadObjectPage(connection, query, after_id, [&](SQLite::Statement& row) {
            output.values.push_back(ReadConverted(row));
            return true;
        });
        return output;
    });
}
Values<ConvertedObject> CacheCatalog::PageConvertedScope(std::string_view scope, std::optional<std::uint64_t> after_id,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(scope);
        auto& query = connection.Query("SELECT file_id,instance,scope,cache_version,model,container,reusable FROM objects "
            "WHERE file_id BETWEEN ?1 AND ?2 AND scope=?4 ORDER BY file_id LIMIT ?3"_sv);
        BindText(query, 4, scope);
        Values<ConvertedObject> output{.result = Success(Outcome::kFound)};
        ReadObjectPage(connection, query, after_id, [&](SQLite::Statement& row) {
            output.values.push_back(ReadConverted(row));
            return true;
        });
        return output;
    });
}
Values<bool> CacheCatalog::OwnsFile(std::uint64_t file_id, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<bool> {
        auto& query = connection.Query("SELECT EXISTS(SELECT 1 FROM objects WHERE file_id=?)"_sv);
        query.bind(1, SignedId(file_id)); query.executeStep();
        return {.result = Success(Outcome::kFound), .values = {StoredBool(query, 0)}};
    });
}
Result CacheCatalog::RevokeConverted(const CacheLocation& location, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        connection.Exec("BEGIN IMMEDIATE"_sv);
        auto& existing = connection.Query("SELECT 1 FROM objects WHERE file_id=? AND instance=?"_sv);
        BindLocation(existing, 1, location);
        if (!existing.executeStep()) return Success(Outcome::kRejected);
        auto& sources = connection.Query("UPDATE sources SET file_id=NULL,instance=NULL WHERE file_id=? AND instance=?"_sv);
        BindLocation(sources, 1, location); sources.exec();
        auto& object = connection.Query("UPDATE objects SET reusable=0 WHERE file_id=? AND instance=?"_sv);
        BindLocation(object, 1, location); object.exec();
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Result CacheCatalog::ForgetConverted(const CacheLocation& location, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        auto& query = connection.Query("DELETE FROM objects WHERE file_id=?1 AND instance=?2 AND reusable=0 "
            "AND NOT EXISTS(SELECT 1 FROM sources WHERE file_id=?1 AND instance=?2) RETURNING file_id"_sv);
        BindLocation(query, 1, location);
        const auto found = query.executeStep();
        if (found && query.executeStep()) throw SQLite::Exception("Multiple converted objects", SQLITE_CORRUPT);
        return Success(found ? Outcome::kCommitted : Outcome::kRejected);
    });
}
Values<CacheMaintenance> CacheCatalog::ReadMaintenance(const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<CacheMaintenance> {
        return {.result = Success(Outcome::kFound), .values = {ReadMaintenanceState(connection)}};
    });
}
Result CacheCatalog::ResetMaintenance(bool request_all, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        connection.Exec("BEGIN IMMEDIATE"_sv);
        const auto state = ReadMaintenanceState(connection);
        if (state.reset_revision == (std::numeric_limits<std::uint64_t>::max)())
            return Error(Failure::kCapacity, "Cache maintenance revision exhausted"_sv);
        auto& query = connection.Query("UPDATE maintenance SET last_success=NULL,reset_revision=?,pending_all=? WHERE id=1"_sv);
        query.bind(1, SignedId(state.reset_revision + 1)); query.bind(2, static_cast<int>(request_all || state.pending_all)); query.exec();
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Result CacheCatalog::CompleteMaintenance(std::uint64_t accepted_revision, std::int64_t utc_seconds,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        if (utc_seconds < 0) throw std::invalid_argument("Negative maintenance completion time");
        auto& query = connection.Query("UPDATE maintenance SET last_success=?,pending_all=0 WHERE id=1 AND reset_revision=? RETURNING id"_sv);
        query.bind(1, utc_seconds); query.bind(2, SignedId(accepted_revision));
        const bool found = query.executeStep();
        if (found && query.executeStep()) throw SQLite::Exception("Multiple maintenance records", SQLITE_CORRUPT);
        return Success(found ? Outcome::kCommitted : Outcome::kRejected);
    });
}

}  // namespace ysm::storage
