#include <storage/file_id_allocator.h>

#include "cache_queries.h"

#include <limits>
#include <sqlite3.h>

namespace ysm::storage {

using detail::CheckTable;
using detail::Connection;
using detail::Error;
using detail::Layout;
using detail::OpenState;
using detail::RequireLayout;
using detail::Scalar;
using detail::SignedId;
using detail::StoredBool;
using detail::Success;
using detail::UnsignedId;

namespace {

void CheckAllocatorTable(SQLite::Database& db) {
    CheckTable(db, "allocator"_sv, {{"id"_sv, "INTEGER"_sv, 0, 1}, {"next_id"_sv, "INTEGER"_sv, 1, 0}, {"exhausted"_sv, "INTEGER"_sv, 1, 0}});
    RequireLayout(Scalar(db, "SELECT count(*) FROM allocator WHERE id=1 AND exhausted IN(0,1)"_sv) == 1,
        "Missing allocator state"_sv);
}

constexpr Layout kLayout{
    .filename = "ids.sqlite"_sv,
    .role = 8,
    .epoch = 1,
    .revision = 1,
    .journal_mode = "wal"_sv,
    .schema = "CREATE TABLE allocator(id INTEGER PRIMARY KEY CHECK(id=1),next_id INTEGER NOT NULL,"
            "exhausted INTEGER NOT NULL CHECK(exhausted IN(0,1))) STRICT;"
            "INSERT INTO allocator VALUES(1,1,0);"_sv,
    .validate_tables = CheckAllocatorTable,
};

}  // namespace

class FileIdAllocator::Impl { public: std::unique_ptr<Connection> connection; };
YSM_PIMPL_DEFINITION(FileIdAllocator)
FileIdAllocator::FileIdAllocator() = default;
FileIdAllocator::~FileIdAllocator() = default;
std::pair<std::unique_ptr<FileIdAllocator>, Result> FileIdAllocator::Open(
    const std::filesystem::path& root, const CancellationState& cancelled) {
    auto owner = std::unique_ptr<FileIdAllocator>(new FileIdAllocator());
    auto [connection, result] = OpenState(root, kLayout, cancelled);
    if (!connection) return {nullptr, result};
    owner->Pimpl().connection = std::move(connection);
    return {std::move(owner), result};
}

Values<std::uint64_t> FileIdAllocator::Allocate(const CancellationState& cancelled) {
    return Pimpl().connection->Run(cancelled, [&](Connection& connection) -> Values<std::uint64_t> {
        connection.Exec("BEGIN IMMEDIATE"_sv);
        auto& query = connection.Query("SELECT next_id,exhausted FROM allocator WHERE id=1"_sv);
        if (!query.executeStep()) throw SQLite::Exception("Missing allocator state", SQLITE_CORRUPT);
        const auto id = UnsignedId(query, 0);
        if (StoredBool(query, 1)) return {.result = Error(Failure::kCapacity, "Cache file ID space exhausted"_sv)};
        auto& update = connection.Query("UPDATE allocator SET next_id=?,exhausted=? WHERE id=1"_sv);
        const bool exhausted = id == (std::numeric_limits<std::uint64_t>::max)();
        update.bind(1, SignedId(exhausted ? id : id + 1)); update.bind(2, static_cast<int>(exhausted)); update.exec();
        Values<std::uint64_t> output{.result = Success(), .values = {id}};
        connection.Exec("COMMIT"_sv);
        return output;
    });
}

}  // namespace ysm::storage
