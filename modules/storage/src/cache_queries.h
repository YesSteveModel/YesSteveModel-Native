#pragma once

#include "database.h"

#include <limits>
#include <optional>

#include <storage/cache_types.h>

namespace ysm::storage::detail {

std::int64_t SignedId(std::uint64_t value);
std::uint64_t UnsignedId(SQLite::Statement& query, int column);
bool StoredBool(SQLite::Statement& query, int column);
void CheckInstance(std::string_view token);
CacheLocation ReadLocation(SQLite::Statement& query, int column = 0);
void BindLocation(SQLite::Statement& query, int column, const CacheLocation& location);

template <typename Emit>
void ReadObjectPage(Connection& connection, SQLite::Statement& query, std::optional<std::uint64_t> after_id,
    Emit&& emit) {
    if (after_id == (std::numeric_limits<std::uint64_t>::max)()) return;

    int remaining = kMaximumRows;
    const auto read_range = [&](std::int64_t first, std::int64_t last) {
        query.bind(1, first);
        query.bind(2, last);
        query.bind(3, remaining);
        while (query.executeStep()) {
            if (!emit(query)) {
                query.reset();
                return false;
            }
            --remaining;
        }
        query.reset();
        return remaining > 0;
    };

    const auto first = SignedId(after_id ? *after_id + 1 : 0);
    if (first < 0) {
        read_range(first, -1);
        return;
    }

    // The unsigned order wraps at the sign bit. Both ranges of a page must see the same snapshot.
    connection.Exec("BEGIN"_sv);
    if (read_range(first, (std::numeric_limits<std::int64_t>::max)()))
        read_range((std::numeric_limits<std::int64_t>::min)(), -1);
    connection.Exec("COMMIT"_sv);
}

void CheckCacheTables(SQLite::Database& db);
Values<CacheFile> LookupCacheFile(Connection& connection, ArtifactKind kind, std::string_view key);
Result PublishCacheFile(Connection& connection, ArtifactKind kind, std::string_view key, const CacheFile& file);
Values<ContentReferences> ReadReferences(Connection& connection, std::span<const std::string> containers,
    const CancellationState& cancelled, CStringView coverage_sql, CStringView reference_sql);

}  // namespace ysm::storage::detail
