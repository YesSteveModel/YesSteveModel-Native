#include <storage/coordinator.h>

#include "database.h"

#include <sqlite3.h>

namespace ysm::storage {

using detail::CheckLocation;
using detail::Error;
using detail::Exception;
using detail::FailureLog;
using detail::Hooks;
using detail::Layout;
using detail::NativePath;
using detail::OpenConnection;
using detail::Scalar;
using detail::Success;
using detail::Wait;

namespace {

constexpr std::string_view kMarkerSchema = "CREATE TABLE marker(value INTEGER PRIMARY KEY CHECK(value=1)) STRICT;INSERT INTO marker(value) VALUES(1);"_sv;

constexpr Layout kAdmissionLayout{
    .filename = "admission.sqlite"_sv,
    .role = 6,
    .epoch = 0,
    .revision = 1,
    .journal_mode = "delete"_sv,
    .schema = kMarkerSchema,
};

constexpr Layout kConsumersLayout{
    .filename = "consumers.sqlite"_sv,
    .role = 7,
    .epoch = 0,
    .revision = 1,
    .journal_mode = "delete"_sv,
    .schema = kMarkerSchema,
};

}  // namespace

class Coordinator::Impl {
public:
    std::unique_ptr<SQLite::Database> admission;
    std::unique_ptr<SQLite::Database> consumers;
    std::filesystem::path scope;
    bool shared = false;
    bool gate = false;
    bool exclusive = false;
    bool broken = false;
    void Shared() {
        consumers->exec("BEGIN");
        try { Scalar(*consumers, "SELECT value FROM marker"_sv); shared = true; }
        catch (...) {
            sqlite3_exec(consumers->getHandle(), "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
};
YSM_PIMPL_DEFINITION(Coordinator)
Coordinator::Coordinator() = default;
Coordinator::~Coordinator() {
    auto& impl = Pimpl();
    if (impl.consumers) sqlite3_exec(impl.consumers->getHandle(), "ROLLBACK", nullptr, nullptr, nullptr);
    if (impl.admission && impl.gate) sqlite3_exec(impl.admission->getHandle(), "ROLLBACK", nullptr, nullptr, nullptr);
}
std::pair<std::unique_ptr<Coordinator>, Result> Coordinator::Open(
    const std::filesystem::path& scope, const CancellationState& cancelled) {
    auto resource = std::unique_ptr<Coordinator>(new Coordinator());
    Wait wait{&cancelled};
    try {
        if (wait.stop()) return {nullptr, Error(Failure::kCancelled, "Registration cancelled"_sv)};
        auto& impl = resource->Pimpl();
        const auto admission_path = NativePath(scope / kAdmissionLayout.filename);
        // Cleanup must check the same path spelling that was used to open the databases.
        impl.scope = admission_path.parent_path();
        impl.admission = OpenConnection(admission_path, kAdmissionLayout, wait);
        Hooks admission_hooks(*impl.admission, wait);
        impl.admission->exec("BEGIN IMMEDIATE"); impl.gate = true;
        impl.consumers = OpenConnection(impl.scope / kConsumersLayout.filename, kConsumersLayout, wait);
        Hooks consumer_hooks(*impl.consumers, wait);
        impl.Shared();
        if (wait.stop()) return {nullptr, Error(Failure::kCancelled, "Registration cancelled"_sv)};
        impl.admission->exec("ROLLBACK"); impl.gate = false;
        return {std::move(resource), Success(Outcome::kFound)};
    } catch (const SQLite::Exception& exception) {
        const auto code = exception.getErrorCode() & 0xff;
        return {nullptr, Exception(exception, &wait,
            code == SQLITE_BUSY || code == SQLITE_LOCKED ? FailureLog::kDebug : FailureLog::kWarning)};
    } catch (const std::exception& exception) { return {nullptr, Error(Failure::kAccessIo, exception.what())}; }
}
Result Coordinator::BeginCleanup(const CancellationState& cancelled) {
    auto& impl = Pimpl();
    if (impl.broken || impl.gate || !impl.shared) return Error(Failure::kClosed, "Coordination is not available"_sv);
    Wait wait{&cancelled};
    if (wait.stop()) return Error(Failure::kCancelled, "Cleanup cancelled"_sv);
    Hooks hooks(*impl.admission, wait);
    try {
        CheckLocation(impl.scope / kAdmissionLayout.filename, false);
        CheckLocation(impl.scope / kConsumersLayout.filename, false);
        impl.admission->exec("BEGIN IMMEDIATE"); impl.gate = true;
        impl.consumers->exec("ROLLBACK"); impl.shared = false;
        try {
            // Another consumer skips cleanup immediately; it does not consume the
            // admission wait budget while this process holds admission.
            impl.consumers->exec("BEGIN EXCLUSIVE"); impl.exclusive = true;
            return Success(Outcome::kFound);
        } catch (const SQLite::Exception& exception) {
            auto result = Exception(exception, &wait);
            const auto restored = EndCleanup();
            if (restored.outcome == Outcome::kFailed) return restored;
            if (result.failure == Failure::kResourceBusy) result.outcome = Outcome::kRejected;
            return result;
        }
    } catch (const SQLite::Exception& exception) {
        auto result = Exception(exception, &wait);
        if (impl.gate) impl.broken = true;
        // Admission contention leaves the original SHARED protection intact.
        else if (result.failure == Failure::kResourceBusy) result.outcome = Outcome::kRejected;
        return result;
    } catch (const std::filesystem::filesystem_error& exception) {
        impl.broken = true;
        return Error(Failure::kAccessIo, exception.what());
    }
}
Result Coordinator::EndCleanup() {
    auto& impl = Pimpl();
    if (impl.broken || !impl.gate) return Error(Failure::kClosed, "No cleanup admission is held"_sv);
    // Restoration has a fresh bounded wait context and never reads the original
    // cancellation state. Admission stays held until protection is restored.
    Wait recovery;
    Hooks admission_hooks(*impl.admission, recovery);
    Hooks consumer_hooks(*impl.consumers, recovery);
    try {
        CheckLocation(impl.scope / kAdmissionLayout.filename, false);
        CheckLocation(impl.scope / kConsumersLayout.filename, false);
        if (impl.exclusive) { impl.consumers->exec("ROLLBACK"); impl.exclusive = false; }
        if (!impl.shared) impl.Shared();
        impl.admission->exec("ROLLBACK"); impl.gate = false;
        return Success();
    } catch (const SQLite::Exception& exception) {
        impl.broken = true;
        return Exception(exception);
    } catch (const std::filesystem::filesystem_error& exception) {
        impl.broken = true;
        return Error(Failure::kAccessIo, exception.what());
    }
}

}  // namespace ysm::storage
