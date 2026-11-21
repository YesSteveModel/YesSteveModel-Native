#include <storage/bake_index.h>

#include "cache_queries.h"

#include <stdexcept>
#include <sqlite3.h>

namespace ysm::storage {

using detail::BindBlob;
using detail::BindLocation;
using detail::BindText;
using detail::Blob;
using detail::CheckInstance;
using detail::CheckKey;
using detail::CheckPayload;
using detail::CheckTable;
using detail::Connection;
using detail::Error;
using detail::Layout;
using detail::OpenState;
using detail::ReadLocation;
using detail::ReadObjectPage;
using detail::RequireLayout;
using detail::Scalar;
using detail::SignedId;
using detail::StoredBool;
using detail::Success;
using detail::Text;

namespace {

void CheckBakeTables(SQLite::Database& db) {
    CheckTable(db, "objects"_sv, {{"file_id"_sv, "INTEGER"_sv, 0, 1}, {"instance"_sv, "TEXT"_sv, 1, 0},
        {"kind"_sv, "INTEGER"_sv, 1, 0}, {"cache_version"_sv, "TEXT"_sv, 1, 0}, {"container"_sv, "TEXT"_sv, 1, 0},
        {"binding"_sv, "TEXT"_sv, 1, 0}, {"purpose"_sv, "TEXT"_sv, 1, 0}, {"exact_key"_sv, "TEXT"_sv, 1, 0},
        {"abi"_sv, "TEXT"_sv, 1, 0}, {"capability"_sv, "TEXT"_sv, 1, 0},
        {"engine_package"_sv, "TEXT"_sv, 1, 0}, {"compilation"_sv, "TEXT"_sv, 1, 0}, {"frozen"_sv, "TEXT"_sv, 1, 0},
        {"value"_sv, "BLOB"_sv, 1, 0}, {"reusable"_sv, "INTEGER"_sv, 1, 0}});
    RequireLayout(Scalar(db, "SELECT count(*) FROM pragma_index_list('objects') WHERE name='reusable_key' AND \"unique\"=1 AND partial=1"_sv) == 1,
        "Missing bake exact identity index"_sv);
}

constexpr Layout kLayout{
    .filename = "bake.sqlite"_sv,
    .role = 11,
    .epoch = 1,
    .revision = 1,
    .journal_mode = "delete"_sv,
    .schema = "CREATE TABLE objects(file_id INTEGER PRIMARY KEY,instance TEXT NOT NULL,kind INTEGER NOT NULL,"
            "cache_version TEXT NOT NULL,container TEXT NOT NULL,binding TEXT NOT NULL,purpose TEXT NOT NULL,"
            "exact_key TEXT NOT NULL,abi TEXT NOT NULL,capability TEXT NOT NULL,"
            "engine_package TEXT NOT NULL,compilation TEXT NOT NULL,frozen TEXT NOT NULL,value BLOB NOT NULL,"
            "reusable INTEGER NOT NULL CHECK(reusable IN(0,1)),UNIQUE(file_id,instance)) STRICT;"
            "CREATE UNIQUE INDEX reusable_key ON objects(kind,cache_version,container,binding,purpose,exact_key,"
            "abi,capability,engine_package,compilation,frozen) WHERE reusable=1;"_sv,
    .validate_tables = CheckBakeTables,
};

void CheckPartition(const BakePartition& partition) {
    if (partition.kind != BakeKind::kGeometry && partition.kind != BakeKind::kAnimation)
        throw std::invalid_argument("Invalid bake kind");
    CheckKey(partition.cache_version); CheckKey(partition.container); CheckKey(partition.binding); CheckKey(partition.purpose);
}
void BindPartition(SQLite::Statement& query, const BakePartition& partition) {
    CheckPartition(partition);
    query.bind(1, static_cast<int>(partition.kind)); BindText(query, 2, partition.cache_version);
    BindText(query, 3, partition.container); BindText(query, 4, partition.binding); BindText(query, 5, partition.purpose);
}
void CheckBakeKey(const BakeKey& key) {
    CheckPartition(key.partition); CheckKey(key.exact); CheckKey(key.abi); CheckKey(key.capability);
    CheckKey(key.engine_package); CheckKey(key.compilation); CheckKey(key.frozen);
}
BakeObject ReadBakeObject(SQLite::Statement& query) {
    BakeObject value;
    value.file = {ReadLocation(query), Blob(query, 13)};
    const auto kind = query.getColumn(2);
    if (!kind.isInteger() || kind.getInt64() < 0 || kind.getInt64() > 1)
        throw SQLite::Exception("Invalid bake kind", SQLITE_CORRUPT);
    value.key = {{static_cast<BakeKind>(kind.getInt64()), Text(query, 3), Text(query, 4), Text(query, 5), Text(query, 6)},
        Text(query, 7), Text(query, 8), Text(query, 9), Text(query, 10), Text(query, 11), Text(query, 12)};
    return value;
}

}  // namespace

class BakeIndex::Impl { public: std::unique_ptr<Connection> connection; bool read_only = false; };
YSM_PIMPL_DEFINITION(BakeIndex)
BakeIndex::BakeIndex() = default;
BakeIndex::~BakeIndex() = default;
std::pair<std::unique_ptr<BakeIndex>, Result> BakeIndex::Open(
    const std::filesystem::path& root, bool read_only, const CancellationState& cancelled) {
    auto owner = std::unique_ptr<BakeIndex>(new BakeIndex());
    auto [connection, result] = OpenState(root, kLayout, cancelled, read_only);
    if (!connection) return {nullptr, result};
    owner->Pimpl().connection = std::move(connection);
    owner->Pimpl().read_only = read_only;
    return {std::move(owner), result};
}

Values<CacheFile> BakeIndex::LookupExact(const BakeKey& key, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<CacheFile> {
        CheckBakeKey(key);
        auto& query = connection.Query("SELECT file_id,instance,value FROM objects WHERE kind=? AND cache_version=? AND container=? AND binding=? AND purpose=? AND exact_key=? "
            "AND abi=? AND capability=? AND engine_package=? AND compilation=? AND frozen=? AND reusable=1"_sv);
        BindPartition(query, key.partition); BindText(query, 6, key.exact); BindText(query, 7, key.abi);
        BindText(query, 8, key.capability); BindText(query, 9, key.engine_package); BindText(query, 10, key.compilation); BindText(query, 11, key.frozen);
        if (!query.executeStep()) return {.result = Success(Outcome::kMissing)};
        return {.result = Success(Outcome::kFound), .values = {{ReadLocation(query), Blob(query, 2)}}};
    });
}
Values<bool> BakeIndex::HasPartition(const BakePartition& partition, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<bool> {
        auto& query = connection.Query("SELECT EXISTS(SELECT 1 FROM objects WHERE kind=? AND cache_version=? AND container=? AND binding=? AND purpose=? AND reusable=1)"_sv);
        BindPartition(query, partition); query.executeStep();
        return {.result = Success(Outcome::kFound), .values = {StoredBool(query, 0)}};
    });
}
Values<bool> BakeIndex::OwnsFile(std::uint64_t file_id, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<bool> {
        auto& query = connection.Query("SELECT EXISTS(SELECT 1 FROM objects WHERE file_id=?)"_sv);
        query.bind(1, SignedId(file_id)); query.executeStep();
        return {.result = Success(Outcome::kFound), .values = {StoredBool(query, 0)}};
    });
}
Result BakeIndex::Publish(const BakeObject& object, const CancellationState& cancelled) {
    if (Pimpl().read_only) return Error(Failure::kAccessIo, "Read-only bake index"_sv);
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        const auto& key = object.key; CheckBakeKey(key); CheckPayload(object.file.metadata); CheckInstance(object.file.location.instance);
        connection.Exec("BEGIN IMMEDIATE"_sv);
        // A retained production is immutable. Re-publication must consume a new
        // allocation, so stale feedback cannot be revived by an old producer.
        auto& existing = connection.Query("SELECT 1 FROM objects WHERE file_id=?"_sv); existing.bind(1, SignedId(object.file.location.file_id));
        if (existing.executeStep()) return Success(Outcome::kRejected);
        auto& retire = connection.Query("UPDATE objects SET reusable=0 WHERE kind=? AND cache_version=? AND container=? AND binding=? AND purpose=? AND exact_key=? "
            "AND abi=? AND capability=? AND engine_package=? AND compilation=? AND frozen=? AND reusable=1"_sv);
        BindPartition(retire, key.partition); BindText(retire, 6, key.exact); BindText(retire, 7, key.abi);
        BindText(retire, 8, key.capability); BindText(retire, 9, key.engine_package); BindText(retire, 10, key.compilation); BindText(retire, 11, key.frozen); retire.exec();
        auto& insert = connection.Query("INSERT INTO objects(kind,cache_version,container,binding,purpose,exact_key,abi,capability,engine_package,compilation,frozen,file_id,instance,value,reusable) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)"_sv);
        BindPartition(insert, key.partition); BindText(insert, 6, key.exact); BindText(insert, 7, key.abi);
        BindText(insert, 8, key.capability); BindText(insert, 9, key.engine_package); BindText(insert, 10, key.compilation); BindText(insert, 11, key.frozen);
        BindLocation(insert, 12, object.file.location); BindBlob(insert, 14, object.file.metadata); insert.exec();
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Values<BakeObject> BakeIndex::PageObjects(std::optional<std::uint64_t> after_id, const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        auto& query = connection.Query("SELECT file_id,instance,kind,cache_version,container,binding,purpose,exact_key,abi,capability,engine_package,compilation,frozen,value FROM objects "
            "WHERE file_id BETWEEN ?1 AND ?2 ORDER BY file_id LIMIT ?3"_sv);
        Values<BakeObject> output{.result = Success(Outcome::kFound)};
        std::size_t bytes = 0;
        ReadObjectPage(connection, query, after_id, [&](SQLite::Statement& row) {
            auto value = ReadBakeObject(row);
            if (bytes + value.file.metadata.size() > kMaximumBytes && !output.values.empty()) return false;
            bytes += value.file.metadata.size();
            output.values.push_back(std::move(value));
            return true;
        });
        return output;
    });
}
Result BakeIndex::Revoke(const CacheLocation& location, const CancellationState& cancelled) {
    if (Pimpl().read_only) return Error(Failure::kAccessIo, "Read-only bake index"_sv);
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        connection.Exec("BEGIN IMMEDIATE"_sv);
        auto& existing = connection.Query("SELECT 1 FROM objects WHERE file_id=? AND instance=?"_sv); BindLocation(existing, 1, location);
        if (!existing.executeStep()) return Success(Outcome::kRejected);
        auto& object = connection.Query("UPDATE objects SET reusable=0 WHERE file_id=? AND instance=?"_sv); BindLocation(object, 1, location); object.exec();
        connection.Exec("COMMIT"_sv); return Success();
    });
}
Result BakeIndex::Forget(const CacheLocation& location, const CancellationState& cancelled) {
    if (Pimpl().read_only) return Error(Failure::kAccessIo, "Read-only bake index"_sv);
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) {
        auto& query = connection.Query("DELETE FROM objects WHERE file_id=?1 AND instance=?2 AND reusable=0 RETURNING file_id"_sv);
        BindLocation(query, 1, location); const bool found = query.executeStep();
        if (found && query.executeStep()) throw SQLite::Exception("Multiple bake objects", SQLITE_CORRUPT);
        return Success(found ? Outcome::kCommitted : Outcome::kRejected);
    });
}

}  // namespace ysm::storage
