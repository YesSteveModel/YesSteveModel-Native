#include "cache_queries.h"

#include <bit>
#include <stdexcept>

#include <sqlite3.h>

namespace ysm::storage::detail {

std::int64_t SignedId(std::uint64_t value) { return std::bit_cast<std::int64_t>(value); }
std::uint64_t UnsignedId(SQLite::Statement& query, int column) {
    if (!query.getColumn(column).isInteger()) throw SQLite::Exception("Invalid integer ID", SQLITE_CORRUPT);
    return std::bit_cast<std::uint64_t>(query.getColumn(column).getInt64());
}
bool StoredBool(SQLite::Statement& query, int column) {
    const auto value = query.getColumn(column);
    if (!value.isInteger() || (value.getInt64() != 0 && value.getInt64() != 1))
        throw SQLite::Exception("Invalid stored boolean", SQLITE_CORRUPT);
    return value.getInt64() != 0;
}
void CheckInstance(std::string_view token) {
    CheckKey(token);
    if (token.size() != 36) throw std::invalid_argument("Invalid production token");
}
CacheLocation ReadLocation(SQLite::Statement& query, int column) {
    auto token = Text(query, column + 1);
    if (token.size() != 36) throw SQLite::Exception("Invalid production token", SQLITE_CORRUPT);
    return {UnsignedId(query, column), std::move(token)};
}
void BindLocation(SQLite::Statement& query, int column, const CacheLocation& location) {
    CheckInstance(location.instance);
    query.bind(column, SignedId(location.file_id)); BindText(query, column + 1, location.instance);
}

void CheckCacheTables(SQLite::Database& db) {
    CheckTable(db, "exact"_sv, {{"kind"_sv, "INTEGER"_sv, 1, 1}, {"key"_sv, "TEXT"_sv, 1, 2},
        {"file_id"_sv, "INTEGER"_sv, 1, 0}, {"instance"_sv, "TEXT"_sv, 1, 0}, {"value"_sv, "BLOB"_sv, 1, 0}});
    CheckTable(db, "coverage"_sv, {{"id"_sv, "INTEGER"_sv, 0, 1}, {"complete"_sv, "INTEGER"_sv, 1, 0}});
    RequireLayout(Scalar(db, "SELECT count(*) FROM coverage WHERE id=1 AND complete IN(0,1)"_sv) == 1,
        "Missing source domain coverage"_sv);
}

Values<CacheFile> LookupCacheFile(Connection& connection, ArtifactKind kind, std::string_view key) {
    CheckKey(key);
    auto& query = connection.Query("SELECT file_id,instance,value FROM exact WHERE kind=? AND key=?"_sv);
    query.bind(1, static_cast<int>(kind)); BindText(query, 2, key);
    if (!query.executeStep()) return {.result = Success(Outcome::kMissing)};
    return {.result = Success(Outcome::kFound), .values = {{ReadLocation(query), Blob(query, 2)}}};
}
Result PublishCacheFile(Connection& connection, ArtifactKind kind, std::string_view key, const CacheFile& file) {
    CheckKey(key); CheckPayload(file.metadata);
    auto& query = connection.Query("INSERT INTO exact(kind,key,file_id,instance,value) VALUES(?,?,?,?,?) "
        "ON CONFLICT(kind,key) DO UPDATE SET file_id=excluded.file_id,instance=excluded.instance,value=excluded.value"_sv);
    query.bind(1, static_cast<int>(kind)); BindText(query, 2, key); BindLocation(query, 3, file.location);
    BindBlob(query, 5, file.metadata); query.exec();
    return Success();
}

Values<ContentReferences> ReadReferences(Connection& connection, std::span<const std::string> containers,
    const CancellationState& cancelled, CStringView coverage_sql, CStringView reference_sql) {
    if (containers.empty() || containers.size() > kMaximumRows)
        throw std::invalid_argument("Invalid reference batch");
    for (const auto& container : containers) CheckKey(container);

    connection.Exec("BEGIN"_sv);
    auto& coverage = connection.Query(coverage_sql);
    if (!coverage.executeStep()) throw SQLite::Exception("Missing reference coverage", SQLITE_CORRUPT);
    const auto root_complete = StoredBool(coverage, 0);
    const auto domains = EventTime(coverage, 1), incomplete_scopes = EventTime(coverage, 2);
    if (incomplete_scopes >= domains) throw SQLite::Exception("Invalid reference coverage", SQLITE_CORRUPT);
    const auto incomplete = incomplete_scopes + !root_complete;
    coverage.reset();
    const auto absent = incomplete ? ReferenceState::kUnknown : ReferenceState::kUnreferenced;
    Values<ContentReferences> output{.result = Success(Outcome::kFound)};
    output.values.reserve(containers.size());
    auto& query = connection.Query(reference_sql);
    for (const auto& container : containers) {
        if (cancelled.IsRequested()) return {.result = Error(Failure::kCancelled, "Operation cancelled"_sv)};
        BindText(query, 1, container);
        if (!query.executeStep()) throw SQLite::Exception("Missing reference lookup result", SQLITE_CORRUPT);
        output.values.push_back({StoredBool(query, 0) ? ReferenceState::kReferenced : absent, domains, incomplete});
        query.reset();
    }
    if (cancelled.IsRequested()) return {.result = Error(Failure::kCancelled, "Operation cancelled"_sv)};
    connection.Exec("COMMIT"_sv);
    return output;
}

}  // namespace ysm::storage::detail
