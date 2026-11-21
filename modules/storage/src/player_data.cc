#include <storage/player_data.h>

#include "database.h"

#include <stdexcept>
#include <sqlite3.h>

namespace ysm::storage {

using detail::BindBlob;
using detail::BindText;
using detail::Blob;
using detail::CheckKey;
using detail::CheckPayload;
using detail::CheckTable;
using detail::Connection;
using detail::EventTime;
using detail::FailureLog;
using detail::Layout;
using detail::OpenState;
using detail::Success;
using detail::Text;

namespace {

void CheckPreferencesTable(SQLite::Database& db) {
    CheckTable(db, "preferences"_sv, {{"uuid"_sv, "TEXT"_sv, 1, 1}, {"kind"_sv, "INTEGER"_sv, 1, 2},
        {"identity"_sv, "TEXT"_sv, 1, 3}, {"format"_sv, "INTEGER"_sv, 1, 0}, {"value"_sv, "BLOB"_sv, 1, 0},
        {"updated"_sv, "INTEGER"_sv, 1, 0}});
}

constexpr Layout kLayout{
    .filename = "preferences-c2.sqlite"_sv,
    .role = 1,
    .epoch = 2,
    .revision = 1,
    .journal_mode = "wal"_sv,
    .schema = "CREATE TABLE preferences(uuid TEXT NOT NULL,kind INTEGER NOT NULL,identity TEXT NOT NULL,"
            "format INTEGER NOT NULL,value BLOB NOT NULL,updated INTEGER NOT NULL,"
            "PRIMARY KEY(uuid,kind,identity)) STRICT;"_sv,
    .validate_tables = CheckPreferencesTable,
    .failure_log = FailureLog::kDebug,
};

void CheckFact(std::string_view player, FactKind kind, std::string_view identity) {
    CheckKey(player); CheckKey(identity);
    const auto number = static_cast<int>(kind);
    if (player.size() != 36 || number < 0 || number > 2 ||
        (kind == FactKind::kLocalSelection ? !identity.empty() : identity.empty()))
        throw std::invalid_argument("Invalid player fact key representation");
}

Values<PlayerFact> ReadFact(Connection& connection, std::string_view player, FactKind kind,
    std::string_view identity) {
    auto& query = connection.Query("SELECT format,value,updated FROM preferences WHERE uuid=? AND kind=? AND identity=?"_sv);
    BindText(query, 1, player); query.bind(2, static_cast<int>(kind)); BindText(query, 3, identity);
    if (!query.executeStep()) return {.result = Success(Outcome::kMissing)};
    if (query.getColumn(0).getInt64() != 1) throw SQLite::Exception("Unknown player fact format", SQLITE_CORRUPT);
    return {.result = Success(Outcome::kFound),
        .values = {{std::string(identity), Blob(query, 1), EventTime(query, 2)}}};
}
Values<PlayerFact> WriteFact(Connection& connection, std::string_view player, FactKind kind,
    std::string_view identity, std::string_view value, std::int64_t utc_seconds, bool first) {
    CheckFact(player, kind, identity); CheckPayload(value);
    if (utc_seconds < 0) throw std::invalid_argument("Negative player fact event time");
    connection.Exec("BEGIN IMMEDIATE"_sv);
    // Preserve uninterpretable facts without copying the old payload for writes.
    auto& existing = connection.Query("SELECT format,typeof(value),length(value),updated FROM preferences "
        "WHERE uuid=? AND kind=? AND identity=?"_sv);
    BindText(existing, 1, player); existing.bind(2, static_cast<int>(kind)); BindText(existing, 3, identity);
    if (existing.executeStep()) {
        if (existing.getColumn(0).getInt64() != 1 || existing.getColumn(1).getString() != "blob" ||
            existing.getColumn(2).getInt64() > kMaximumBytes)
            throw SQLite::Exception("Uninterpretable player fact", SQLITE_CORRUPT);
        EventTime(existing, 3);
    }
    auto& query = connection.Query(first
        ? "INSERT INTO preferences(uuid,kind,identity,format,value,updated) VALUES(?,?,?,1,?,?) "
          "ON CONFLICT(uuid,kind,identity) DO NOTHING"_sv
        : "INSERT INTO preferences(uuid,kind,identity,format,value,updated) VALUES(?,?,?,1,?,?) ON CONFLICT(uuid,kind,identity) "
          "DO UPDATE SET value=excluded.value,updated=excluded.updated"_sv);
    BindText(query, 1, player); query.bind(2, static_cast<int>(kind)); BindText(query, 3, identity);
    BindBlob(query, 4, value); query.bind(5, utc_seconds); query.exec();
    auto result = ReadFact(connection, player, kind, identity);
    connection.Exec("COMMIT"_sv);
    result.result = Success();
    return result;
}

}  // namespace

class PlayerData::Impl { public: std::unique_ptr<Connection> connection; };
YSM_PIMPL_DEFINITION(PlayerData)
PlayerData::PlayerData() = default;
PlayerData::~PlayerData() = default;
std::pair<std::unique_ptr<PlayerData>, Result> PlayerData::Open(
    const std::filesystem::path& root, const CancellationState& cancelled) {
    auto owner = std::unique_ptr<PlayerData>(new PlayerData());
    auto [connection, result] = OpenState(root, kLayout, cancelled);
    if (!connection) return {nullptr, result};
    owner->Pimpl().connection = std::move(connection);
    return {std::move(owner), result};
}
Values<PlayerFact> PlayerData::ReadPreference(std::string_view player, FactKind kind,
    std::string_view identity, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckFact(player, kind, identity);
        return ReadFact(connection, player, kind, identity);
    }, kLayout.failure_log);
}
Values<PlayerFact> PlayerData::PutPreference(std::string_view player, FactKind kind,
    std::string_view identity, std::string_view value, std::int64_t utc_seconds,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        return WriteFact(connection, player, kind, identity, value, utc_seconds, false);
    }, kLayout.failure_log);
}
Values<PlayerFact> PlayerData::InsertIfAbsentAndRead(std::string_view player, FactKind kind,
    std::string_view identity, std::string_view value, std::int64_t utc_seconds,
    const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        return WriteFact(connection, player, kind, identity, value, utc_seconds, true);
    }, kLayout.failure_log);
}
Values<PlayerFact> PlayerData::PagePreferences(std::string_view player, FactKind kind,
    std::string_view after_identity, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        CheckKey(player); CheckKey(after_identity);
        if (player.size() != 36 || (kind != FactKind::kRoaming && kind != FactKind::kFavourite))
            throw std::invalid_argument("Invalid player fact page");
        auto& query = connection.Query("SELECT identity,format,value,updated FROM preferences "
            "WHERE uuid=? AND kind=? AND identity>? ORDER BY identity LIMIT 64"_sv);
        BindText(query, 1, player); query.bind(2, static_cast<int>(kind)); BindText(query, 3, after_identity);
        Values<PlayerFact> result{.result = Success(Outcome::kFound)};
        std::size_t bytes = 0;
        while (query.executeStep()) {
            if (query.getColumn(1).getInt64() != 1) throw SQLite::Exception("Unknown player fact format", SQLITE_CORRUPT);
            auto identity = Text(query, 0);
            auto value = Blob(query, 2);
            if (bytes + value.size() > kMaximumBytes && !result.values.empty()) break;
            bytes += value.size();
            result.values.push_back({std::move(identity), std::move(value), EventTime(query, 3)});
        }
        return result;
    }, kLayout.failure_log);
}

}  // namespace ysm::storage
