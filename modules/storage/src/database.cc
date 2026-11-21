#include "database.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <mimalloc.h>
#include <sqlite3.h>

#include <log.h>
#include <scope_guard.h>

namespace ysm::storage::detail {

std::filesystem::path NativePath(const std::filesystem::path& path) {
    auto native_path = path;
#if defined(YSM_WINDOWS)
    auto name = native_path.make_preferred().native();
    if (!name.starts_with(L"\\\\?\\")) {
        name = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
        if (name.starts_with(L"\\\\")) name = L"\\\\?\\UNC\\" + name.substr(2);
        else name = L"\\\\?\\" + name;
        native_path = name;
    }
#else
    native_path = std::filesystem::absolute(path).lexically_normal();
#endif
    return native_path;
}

namespace {

constexpr int kApplicationBase = 0x59534d00;
constexpr auto kBusyBudget = std::chrono::milliseconds(250);

int Initialize() {
    static std::once_flag once;
    static int result = SQLITE_ERROR;
    std::call_once(once, [] {
        sqlite3_mem_methods methods{};
        methods.xMalloc = [](int size) { return mi_malloc(static_cast<std::size_t>(size)); };
        methods.xFree = mi_free;
        methods.xRealloc = [](void* pointer, int size) {
            return mi_realloc(pointer, static_cast<std::size_t>(size));
        };
        methods.xSize = [](void* pointer) {
            return static_cast<int>(std::min(mi_usable_size(pointer),
                static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        };
        methods.xRoundup = [](int size) { return size; };
        methods.xInit = [](void*) { return SQLITE_OK; };
        methods.xShutdown = [](void*) {};
        result = sqlite3_config(SQLITE_CONFIG_MALLOC, &methods);
        if (result == SQLITE_OK) result = sqlite3_initialize();
#if defined(YSM_WINDOWS)
        if (result == SQLITE_OK) {
            auto* vfs = sqlite3_vfs_find("win32-longpath");
            if (!vfs) result = SQLITE_NOTFOUND;
            else result = sqlite3_vfs_register(vfs, 1);
        }
#endif
    });
    return result;
}

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void CheckIdentity(SQLite::Database& db, const Layout& layout) {
    const auto application = Scalar(db, "PRAGMA application_id"_sv);
    const auto revision = Scalar(db, "PRAGMA user_version"_sv);
    std::int64_t epoch = 0;
    if (layout.epoch != 0 && application == kApplicationBase + layout.role) {
        CheckTable(db, "storage_meta"_sv, {{"id"_sv, "INTEGER"_sv, 0, 1}, {"compatibility_epoch"_sv, "INTEGER"_sv, 1, 0}}, true);
        SQLite::Statement metadata(db, "SELECT id,compatibility_epoch FROM storage_meta");
        RequireLayout(metadata.executeStep() && metadata.getColumn(0).isInteger() &&
            metadata.getColumn(0).getInt64() == 1 && metadata.getColumn(1).isInteger(), "Invalid storage metadata"_sv);
        epoch = metadata.getColumn(1).getInt64();
        RequireLayout(!metadata.executeStep(), "Multiple storage metadata rows"_sv);
    }
    YSM_LOG_DEBUG("Storage role={} expected_epoch={} actual_epoch={} current_revision={} actual_revision={}",
        layout.role, layout.epoch, epoch, layout.revision, revision);
    RequireLayout(application == kApplicationBase + layout.role && epoch == layout.epoch &&
        (layout.epoch == 0 ? revision == layout.revision : revision >= layout.revision),
        std::format("Storage role={} expected_epoch={} actual_epoch={} current_revision={} actual_revision={} "
            "actual_application={} identity or revision mismatch", layout.role, layout.epoch,
            epoch, layout.revision, revision, application));
    if (layout.validate_tables) layout.validate_tables(db);
}

enum class Creation { kExisting, kCreated, kRaced };
Creation ClaimCreation(const std::filesystem::path& path) {
    CheckLocation(path, true);
    if (std::filesystem::exists(path)) return Creation::kExisting;
    // noreplace supplies the creation qualification; an exists check alone
    // cannot authorize initializing a file created by another process.
    std::ofstream created(path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (created) return Creation::kCreated;
    CheckLocation(path, false);
    return Creation::kRaced;
}

}  // namespace

Result Success(Outcome outcome) {
    return {.outcome = outcome, .committed = outcome == Outcome::kCommitted};
}
Result Error(Failure failure, std::string_view diagnostic, int code,
    FailureLog failure_log) {
    if (failure_log == FailureLog::kDebug)
        YSM_LOG_DEBUG("Storage failure={} sqlite_extended={} {}", static_cast<int>(failure), code, diagnostic);
    else
        YSM_LOG(WARNING, "Storage failure={} sqlite_extended={} {}", static_cast<int>(failure), code, diagnostic);
    return {.outcome = failure == Failure::kCancelled ? Outcome::kCancelled : Outcome::kFailed,
        .failure = failure};
}

Wait::Wait(const CancellationState* token)
    : token(token), deadline(std::chrono::steady_clock::now() + kBusyBudget) {}

int Wait::Busy(void* context, int) {
    auto& wait = *static_cast<Wait*>(context);
    if (wait.stop() || std::chrono::steady_clock::now() >= wait.deadline) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return !wait.stop();
}
int Wait::Progress(void* context) { return static_cast<Wait*>(context)->stop(); }

Result Exception(const SQLite::Exception& exception, const Wait* wait,
    FailureLog failure_log) {
    const auto extended = exception.getExtendedErrorCode();
    const auto code = extended > 0 ? extended : exception.getErrorCode();
    Failure failure;
    switch (code & 0xff) {
        case SQLITE_BUSY: case SQLITE_LOCKED: failure = Failure::kResourceBusy; break;
        case SQLITE_SCHEMA: failure = Failure::kIncompatible; break;
        case SQLITE_CORRUPT: case SQLITE_NOTADB: case SQLITE_FORMAT: failure = Failure::kContent; break;
        case SQLITE_INTERRUPT: failure = Failure::kCancelled; break;
        case SQLITE_PERM: case SQLITE_READONLY: case SQLITE_IOERR: case SQLITE_CANTOPEN: case SQLITE_FULL:
            failure = Failure::kAccessIo; break;
        default: failure = Failure::kInternal;
    }
    if (wait && wait->stop() && (failure == Failure::kResourceBusy || failure == Failure::kCancelled))
        failure = Failure::kCancelled;
    return Error(failure, exception.what(), code, failure_log);
}

Hooks::Hooks(SQLite::Database& db, Wait& wait) : db_(db) {
    sqlite3_busy_handler(db_.getHandle(), Wait::Busy, &wait);
    sqlite3_progress_handler(db_.getHandle(), 1000, Wait::Progress, &wait);
}
Hooks::~Hooks() {
    sqlite3_busy_handler(db_.getHandle(), nullptr, nullptr);
    sqlite3_progress_handler(db_.getHandle(), 0, nullptr, nullptr);
}

void CheckKey(std::string_view key) {
    if (key.size() > 4096 || key.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid business key representation");
}
void CheckPayload(std::string_view first, std::string_view second) {
    if (first.size() + second.size() > kMaximumBytes)
        throw std::invalid_argument("Business payload exceeds storage bound");
}
void CheckLocation(const std::filesystem::path& path, bool may_create) {
    const auto parent = std::filesystem::symlink_status(path.parent_path());
    if (parent.type() != std::filesystem::file_type::directory)
        throw std::filesystem::filesystem_error("Storage root is not a real directory", path.parent_path(),
            std::make_error_code(std::errc::not_a_directory));
    const auto target = std::filesystem::symlink_status(path);
    if (target.type() != std::filesystem::file_type::regular &&
        !(may_create && target.type() == std::filesystem::file_type::not_found))
        throw std::filesystem::filesystem_error("Storage target is not a regular file", path,
            std::make_error_code(std::errc::operation_not_permitted));
}
std::string Blob(SQLite::Statement& query, int column) {
    const auto value = query.getColumn(column);
    if (!value.isBlob() || value.getBytes() > static_cast<int>(kMaximumBytes))
        throw SQLite::Exception("Invalid storage blob", SQLITE_CORRUPT);
    if (value.getBytes() == 0) return {};
    return {static_cast<const char*>(value.getBlob()), static_cast<std::size_t>(value.getBytes())};
}
std::string Text(SQLite::Statement& query, int column) {
    const auto value = query.getColumn(column);
    if (!value.isText() || value.getBytes() > 4096)
        throw SQLite::Exception("Invalid storage text", SQLITE_CORRUPT);
    auto text = value.getString();
    if (text.find('\0') != std::string::npos)
        throw SQLite::Exception("Storage text contains NUL", SQLITE_CORRUPT);
    return text;
}
std::int64_t EventTime(SQLite::Statement& query, int column) {
    const auto value = query.getColumn(column);
    if (!value.isInteger() || value.getInt64() < 0)
        throw SQLite::Exception("Invalid stored event time", SQLITE_CORRUPT);
    return value.getInt64();
}
void BindText(SQLite::Statement& query, int position, std::string_view text) {
    query.bind(position, std::string(text));
}
void BindBlob(SQLite::Statement& query, int position, std::string_view value) {
    query.bind(position, value.empty() ? "" : value.data(), static_cast<int>(value.size()));
}
int Scalar(SQLite::Database& db, std::string_view sql) { return db.execAndGet(std::string(sql)).getInt(); }

void RequireLayout(bool valid, std::string_view diagnostic) {
    if (!valid) throw SQLite::Exception(std::string(diagnostic), SQLITE_SCHEMA);
}
void CheckTable(SQLite::Database& db, std::string_view table, std::initializer_list<Column> columns,
    bool exact) {
    SQLite::Statement definition(db, "SELECT strict,wr FROM pragma_table_list WHERE schema='main' AND name=? AND type='table'");
    BindText(definition, 1, table);
    RequireLayout(definition.executeStep() && definition.getColumn(0).getInt() == 1 &&
        definition.getColumn(1).getInt() == 0, "Missing or incompatible storage table"_sv);
    SQLite::Statement fields(db, "SELECT name,type,\"notnull\",pk,dflt_value,hidden FROM pragma_table_xinfo(?)");
    BindText(fields, 1, table);
    std::size_t found = 0;
    std::size_t total = 0;
    while (fields.executeStep()) {
        ++total;
        const auto name = fields.getColumn(0).getString();
        const auto expected = std::find_if(columns.begin(), columns.end(),
            [&](const Column& column) { return column.name == name; });
        if (expected == columns.end()) {
            RequireLayout(!exact && fields.getColumn(3).getInt() == 0, "Unknown storage key column"_sv);
            continue;
        }
        ++found;
        RequireLayout(fields.getColumn(1).getString() == expected->type &&
            fields.getColumn(2).getInt() == expected->not_null &&
            fields.getColumn(3).getInt() == expected->primary_key && fields.getColumn(5).getInt() == 0 &&
            (!exact || fields.getColumn(4).isNull()), "Incompatible storage column"_sv);
    }
    RequireLayout(found == columns.size() && (!exact || total == columns.size()), "Incomplete storage columns"_sv);
}

std::unique_ptr<SQLite::Database> OpenConnection(const std::filesystem::path& path, const Layout& layout, Wait& wait,
    bool read_only, bool existing_only) {
    if (Initialize() != SQLITE_OK) throw std::runtime_error("Private allocator initialization failed");
    const auto native_path = NativePath(path);
    if (read_only || existing_only) CheckLocation(native_path, false);
    const auto creation = read_only || existing_only ? Creation::kExisting : ClaimCreation(native_path);
    auto db = std::make_unique<SQLite::Database>(PathText(native_path),
        (read_only ? SQLite::OPEN_READONLY : SQLite::OPEN_READWRITE) | SQLite::OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW);
    sqlite3_extended_result_codes(db->getHandle(), 1);
    sqlite3_limit(db->getHandle(), SQLITE_LIMIT_LENGTH, static_cast<int>(4 * kMaximumBytes));
    Hooks hooks(*db, wait);
    db->exec(creation == Creation::kExisting ? "BEGIN" : "BEGIN IMMEDIATE");
    auto rollback = ScopeGuard([&] {
        sqlite3_busy_handler(db->getHandle(), nullptr, nullptr);
        sqlite3_progress_handler(db->getHandle(), 0, nullptr, nullptr);
        sqlite3_exec(db->getHandle(), "ROLLBACK", nullptr, nullptr, nullptr);
    });
    const auto application = Scalar(*db, "PRAGMA application_id"_sv);
    const auto version = Scalar(*db, "PRAGMA user_version"_sv);
    if (application == 0 && version == 0 && Scalar(*db, "SELECT count(*) FROM sqlite_schema"_sv) == 0) {
        if (creation == Creation::kRaced)
            throw SQLite::Exception("Concurrent storage initialization has not committed", SQLITE_BUSY);
        RequireLayout(creation == Creation::kCreated, "Existing uninitialized storage database"_sv);
        YSM_LOG(INFO, "Storage role={} stage=initialize epoch={} revision={}",
            layout.role, layout.epoch, layout.revision);
        db->exec(std::string(layout.schema));
        if (layout.epoch != 0) {
            db->exec("CREATE TABLE storage_meta(id INTEGER PRIMARY KEY CHECK(id=1),"
                "compatibility_epoch INTEGER NOT NULL CHECK(compatibility_epoch>0)) STRICT");
            db->exec("INSERT INTO storage_meta(id,compatibility_epoch) VALUES(1," + std::to_string(layout.epoch) + ")");
        }
        db->exec("PRAGMA application_id=" + std::to_string(kApplicationBase + layout.role));
        db->exec("PRAGMA user_version=" + std::to_string(layout.revision));
    } else {
        CheckIdentity(*db, layout);
    }
    if (wait.stop()) throw SQLite::Exception("Storage initialization cancelled", SQLITE_INTERRUPT);
    db->exec("COMMIT");
    rollback.Release();
    const auto expected_mode = layout.journal_mode;
    auto mode = db->execAndGet("PRAGMA journal_mode").getString();
    if (!read_only && mode != expected_mode)
        mode = db->execAndGet(std::string("PRAGMA journal_mode=").append(expected_mode)).getString();
    if (mode != expected_mode) throw SQLite::Exception("Required journal mode unavailable", SQLITE_CANTOPEN);
    if (!read_only) db->exec("PRAGMA synchronous=NORMAL");
    db->exec("PRAGMA foreign_keys=ON");
    db->exec("PRAGMA trusted_schema=OFF");
    return db;
}

Connection::Connection(std::unique_ptr<SQLite::Database> db, std::filesystem::path path)
    : db_(std::move(db)), path_(NativePath(path)) {}

// All callers supply fixed program literals. Entries remain owned by this
// connection, so a worker restart cannot reopen or recreate healthy state.
SQLite::Statement& Connection::Query(std::string_view sql) {
    auto found = statements_.find(sql);
    if (found == statements_.end()) {
        auto prepared = std::make_unique<SQLite::Statement>(*db_, std::string(sql));
        found = statements_.emplace(sql, std::move(prepared)).first;
    } else {
        found->second->tryReset();
        found->second->clearBindings();
    }
    return *found->second;
}
void Connection::Exec(std::string_view sql) { Query(sql).exec(); }

bool Connection::Clean(FailureLog failure_log) {
    bool clean = true;
    for (const auto& [sql, statement] : statements_) {
        // reset reports the preceding step's failure even when the reset
        // succeeds. Busy state and clear-bindings establish reusability.
        statement->tryReset();
    }
    for (auto* statement = sqlite3_next_stmt(db_->getHandle(), nullptr); statement;
         statement = sqlite3_next_stmt(db_->getHandle(), statement)) {
        clean &= !sqlite3_stmt_busy(statement);
        clean &= sqlite3_clear_bindings(statement) == SQLITE_OK;
    }
    if (!sqlite3_get_autocommit(db_->getHandle())) {
        Wait recovery;
        Hooks hooks(*db_, recovery);
        const auto code = sqlite3_exec(db_->getHandle(), "ROLLBACK", nullptr, nullptr, nullptr);
        if (code != SQLITE_OK) Error(Failure::kInternal, "Storage rollback failed"_sv,
            sqlite3_extended_errcode(db_->getHandle()), failure_log);
    }
    return clean && sqlite3_get_autocommit(db_->getHandle());
}

std::pair<std::unique_ptr<Connection>, Result> OpenState(
    const std::filesystem::path& root, const Layout& layout, const CancellationState& cancelled,
    bool read_only, bool existing_only) {
    const auto path = root / layout.filename;
    const auto failure_log = layout.failure_log;
    Wait wait{&cancelled};
    try {
        if (wait.stop()) return {nullptr, Error(Failure::kCancelled, "Open cancelled"_sv, 0, failure_log)};
        if (read_only || existing_only) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(NativePath(path), error);
            if (status.type() == std::filesystem::file_type::not_found &&
                (!error || error == std::errc::no_such_file_or_directory)) return {nullptr, Success(Outcome::kMissing)};
            if (error) throw std::filesystem::filesystem_error("Storage observation failed", path, error);
        }
        auto db = OpenConnection(path, layout, wait, read_only, existing_only);
        return {std::make_unique<Connection>(std::move(db), path), Success(Outcome::kFound)};
    } catch (const SQLite::Exception& exception) {
        if (failure_log == FailureLog::kDebug)
            YSM_LOG_DEBUG("Storage role={} stage=open expected_epoch={} current_revision={} {}",
                layout.role, layout.epoch, layout.revision, exception.what());
        else
            YSM_LOG(WARNING, "Storage role={} stage=open expected_epoch={} current_revision={} {}",
                layout.role, layout.epoch, layout.revision, exception.what());
        return {nullptr, Exception(exception, &wait, failure_log)};
    } catch (const std::filesystem::filesystem_error& exception) {
        return {nullptr, Error(Failure::kAccessIo, exception.what(), 0, failure_log)};
    } catch (const std::exception& exception) {
        return {nullptr, Error(Failure::kInternal, exception.what(), 0, failure_log)};
    }
}

}  // namespace ysm::storage::detail
