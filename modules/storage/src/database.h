#pragma once

#include <chrono>
#include <concepts>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <c_string_view.h>
#include <cancellation.h>
#include <storage/result.h>

namespace ysm::storage::detail {

enum class FailureLog { kWarning, kDebug };

struct Layout {
    std::string_view filename;
    int role;  // Persisted in application_id. Keep existing assignments stable.
    int epoch;  // Zero requires an exact revision and omits storage_meta.
    int revision;
    std::string_view journal_mode;
    std::string_view schema;
    void (*validate_tables)(SQLite::Database&) = nullptr;
    FailureLog failure_log = FailureLog::kWarning;
};

Result Success(Outcome outcome = Outcome::kCommitted);
Result Error(Failure failure, std::string_view diagnostic, int code = 0,
    FailureLog failure_log = FailureLog::kWarning);

struct Wait {
    explicit Wait(const CancellationState* token = nullptr);

    const CancellationState* token;
    std::chrono::steady_clock::time_point deadline;
    bool stop() const { return token && token->IsRequested(); }

    static int Busy(void* context, int);
    static int Progress(void* context);
};

Result Exception(const SQLite::Exception& exception, const Wait* wait = nullptr,
    FailureLog failure_log = FailureLog::kWarning);

class Hooks {
public:
    Hooks(SQLite::Database& db, Wait& wait);
    ~Hooks();
private:
    SQLite::Database& db_;
};

void CheckKey(std::string_view key);
void CheckPayload(std::string_view first, std::string_view second = {});
std::filesystem::path NativePath(const std::filesystem::path& path);
void CheckLocation(const std::filesystem::path& path, bool may_create);
std::string Blob(SQLite::Statement& query, int column);
std::string Text(SQLite::Statement& query, int column);
std::int64_t EventTime(SQLite::Statement& query, int column);
void BindText(SQLite::Statement& query, int position, std::string_view text);
void BindBlob(SQLite::Statement& query, int position, std::string_view value);
int Scalar(SQLite::Database& db, std::string_view sql);

struct Column {
    std::string_view name;
    std::string_view type;
    int not_null;
    int primary_key;
};

void RequireLayout(bool valid, std::string_view diagnostic);
void CheckTable(SQLite::Database& db, std::string_view table, std::initializer_list<Column> columns,
    bool exact = false);

std::unique_ptr<SQLite::Database> OpenConnection(const std::filesystem::path& path,
    const Layout& layout, Wait& wait, bool read_only = false, bool existing_only = false);

template <typename T>
T Failed(Result failure) {
    if constexpr (std::same_as<T, Result>) return failure;
    else return {.result = failure};
}
template <typename T>
Result& Header(T& result) {
    if constexpr (std::same_as<T, Result>) return result;
    else return result.result;
}

class Connection {
public:
    Connection(std::unique_ptr<SQLite::Database> db, std::filesystem::path path);
    SQLite::Statement& Query(std::string_view sql);
    void Exec(std::string_view sql);

    template <typename Function>
    auto Run(const CancellationState& cancelled, Function&& function,
        FailureLog failure_log = FailureLog::kWarning) {
        using T = std::invoke_result_t<Function, Connection&>;
        if (!db_) return Failed<T>(Error(Failure::kClosed, "Business object is no longer reusable"_sv, 0, failure_log));
        if (cancelled.IsRequested()) return Failed<T>(Error(Failure::kCancelled, "Operation cancelled"_sv, 0, failure_log));
        Wait wait{&cancelled};
        T output;
        bool location_valid = true;
        {
            Hooks hooks(*db_, wait);
            try {
                CheckLocation(path_, false);
                output = function(*this);
            } catch (const SQLite::Exception& exception) {
                output = Failed<T>(Exception(exception, &wait, failure_log));
            } catch (const std::filesystem::filesystem_error& exception) {
                location_valid = false;
                output = Failed<T>(Error(Failure::kAccessIo, exception.what(), 0, failure_log));
            } catch (const std::exception& exception) {
                output = Failed<T>(Error(Failure::kInternal, exception.what(), 0, failure_log));
            }
        }
        const auto clean = Clean(failure_log);
        if (!clean || !location_valid) {
            if (!clean) {
                const auto committed = Header(output).committed;
                output = Failed<T>(Error(Failure::kInternal, "Business object could not recover execution state"_sv, 0,
                    failure_log));
                Header(output).committed = committed;
            }
            statements_.clear();
            db_.reset();
        }
        return output;
    }
private:
    bool Clean(FailureLog failure_log);

    std::unique_ptr<SQLite::Database> db_;
    std::filesystem::path path_;
    absl::flat_hash_map<std::string_view, std::unique_ptr<SQLite::Statement>> statements_;
};

std::pair<std::unique_ptr<Connection>, Result> OpenState(
    const std::filesystem::path& root, const Layout& layout, const CancellationState& cancelled,
    bool read_only = false, bool existing_only = false);

}  // namespace ysm::storage::detail
