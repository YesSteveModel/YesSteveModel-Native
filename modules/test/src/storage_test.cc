#include <storage/bake_index.h>
#include <storage/cache_catalog.h>
#include <storage/cache_remote.h>
#include <storage/coordinator.h>
#include <storage/file_id_allocator.h>
#include <storage/player_data.h>

#include <bit>
#include <limits>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <functional>
#include <latch>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>
#include <mimalloc.h>
#include <sqlite3.h>
#include <log.h>
#include <c_string_view.h>
#include <scope_guard.h>

#if defined(YSM_LINUX)
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace ysm::storage {
namespace {
constexpr CStringView kPlayer = "00000000-0000-0000-0000-000000000001"_sv;
class StorageTest : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> sequence{};
        root = std::filesystem::temp_directory_path() /
            ("ysm-storage-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence++));
        std::filesystem::create_directory(root);
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    Values<PlayerFact> Put(PlayerData& owner, std::string_view identity, std::string_view value) {
        return owner.PutPreference(kPlayer, FactKind::kRoaming, identity, value, 0, running);
    }
    Values<PlayerFact> Read(PlayerData& owner, std::string_view identity) {
        return owner.ReadPreference(kPlayer, FactKind::kRoaming, identity, running);
    }
    void Bootstrap() {
        std::filesystem::create_directory(root / "bootstrap");
        auto [owner, result] = PlayerData::Open(root / "bootstrap", running);
        ASSERT_NE(owner, nullptr);
    }
    std::filesystem::path root;
    CancellationState running;
};

// Faults enter through SQLite's VFS, so they exercise the production error and
// transaction paths without adding fault controls to the storage API.
class FaultVfs {
public:
    explicit FaultVfs() {
        original_ = sqlite3_vfs_find(nullptr);
        vfs_ = *original_;
        offset_ = (static_cast<std::size_t>(vfs_.szOsFile) + alignof(File) - 1) & ~(alignof(File) - 1);
        vfs_.szOsFile = static_cast<int>(offset_ + sizeof(File));
        vfs_.zName = "ysm-storage-test-vfs";
        vfs_.pAppData = this;
        vfs_.xOpen = Open;
        current_ = this;
        sqlite3_vfs_register(&vfs_, 1);
    }
    ~FaultVfs() {
        sqlite3_vfs_register(original_, 1);
        sqlite3_vfs_unregister(&vfs_);
        current_ = nullptr;
    }
    bool refuse_shared = false;
    bool disk_full = false;
    bool read_only = false;
    CancellationState* request_on_write = nullptr;
    CancellationState* request_on_busy = nullptr;
    std::function<void()> before_player_open;
    std::function<void()> after_player_main_write;
private:
    struct File {
        sqlite3_io_methods methods;
        const sqlite3_io_methods* original;
        bool consumers;
        bool preferences;
        bool main_database;
    };
    static inline FaultVfs* current_ = nullptr;
    sqlite3_vfs* original_{};
    sqlite3_vfs vfs_{};
    std::size_t offset_{};
    static File& State(sqlite3_file* file) {
        return *reinterpret_cast<File*>(reinterpret_cast<char*>(file) + current_->offset_);
    }
    static int Lock(sqlite3_file* file, int lock) {
        auto& state = State(file);
        if (state.consumers && current_->refuse_shared && lock == SQLITE_LOCK_SHARED) return SQLITE_IOERR_LOCK;
        return state.original->xLock(file, lock);
    }
    static int Write(sqlite3_file* file, const void* bytes, int size, sqlite3_int64 offset) {
        auto& state = State(file);
        if (state.preferences && current_->disk_full) return SQLITE_FULL;
        if (state.preferences && current_->read_only) return SQLITE_READONLY;
        const auto status = state.original->xWrite(file, bytes, size, offset);
        if (status == SQLITE_OK && state.preferences && current_->request_on_write)
            current_->request_on_write->Request();
        if (status == SQLITE_OK && state.preferences && state.main_database && current_->after_player_main_write)
            current_->after_player_main_write();
        return status;
    }
    static int ShmLock(sqlite3_file* file, int offset, int count, int flags) {
        auto& state = State(file);
        const auto status = state.original->xShmLock(file, offset, count, flags);
        if ((status & 0xff) == SQLITE_BUSY && state.preferences && current_->request_on_busy)
            current_->request_on_busy->Request();
        return status;
    }
    static int Open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* output) {
        auto& owner = *static_cast<FaultVfs*>(vfs->pAppData);
        if (name && std::strstr(name, "preferences-c2.sqlite") && (flags & SQLITE_OPEN_MAIN_DB) && owner.before_player_open) {
            const auto callback = std::move(owner.before_player_open);
            owner.before_player_open = {};
            callback();
        }
        const auto status = owner.original_->xOpen(owner.original_, name, file, flags, output);
        if (status != SQLITE_OK) return status;
        auto& state = State(file);
        state.original = file->pMethods;
        state.methods = *file->pMethods;
        state.consumers = name && std::strstr(name, "consumers.sqlite");
        state.preferences = name && std::strstr(name, "preferences-c2.sqlite");
        state.main_database = (flags & SQLITE_OPEN_MAIN_DB) != 0;
        state.methods.xLock = Lock;
        state.methods.xWrite = Write;
        state.methods.xShmLock = ShmLock;
        file->pMethods = &state.methods;
        return SQLITE_OK;
    }
};

class PeerDatabase {
public:
    explicit PeerDatabase(const std::filesystem::path& path) {
        sqlite3* opened = nullptr;
        const auto status = sqlite3_open(path.string().c_str(), &opened);
        db_.reset(opened);
        if (status != SQLITE_OK) throw std::runtime_error("Cannot open fixture database");
        sqlite3_busy_timeout(db_.get(), 1000);
    }
    void Exec(std::string_view sql) {
        if (sqlite3_exec(db_.get(), std::string(sql).c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_.get()));
    }
    std::string Value(std::string_view sql) {
        sqlite3_stmt* prepared = nullptr;
        if (sqlite3_prepare_v2(db_.get(), std::string(sql).c_str(), -1, &prepared, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_.get()));
        const auto finalize = ScopeGuard([&] { sqlite3_finalize(prepared); });
        if (sqlite3_step(prepared) != SQLITE_ROW) throw std::runtime_error("Fixture query returned no row");
        const auto text = sqlite3_column_text(prepared, 0);
        return text ? std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(prepared, 0)) : "";
    }
private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
};

std::string FileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST_F(StorageTest, CreatesAndReopensDatabaseInUnicodeDirectory) {
    const auto scope = root / u8"game-\u6A21\u578B";
    ASSERT_TRUE(std::filesystem::create_directory(scope));

    auto [database, opened] = PlayerData::Open(scope, running);
    ASSERT_EQ(opened.outcome, Outcome::kFound);
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(Put(*database, "owner/model"_sv, "saved"_sv).result.committed);
    database.reset();
    ASSERT_TRUE(std::filesystem::is_regular_file(scope / "preferences-c2.sqlite"));

    auto [reopened, result] = PlayerData::Open(scope, running);
    ASSERT_EQ(result.outcome, Outcome::kFound);
    ASSERT_NE(reopened, nullptr);
    const auto saved = Read(*reopened, "owner/model"_sv);
    ASSERT_EQ(saved.values.size(), 1u);
    EXPECT_EQ(saved.values[0].value, "saved");
}

TEST_F(StorageTest, NewEpochStartsEmptyAndIgnoresDamagedOldPreferences) {
    const auto old_file = root / "preferences.sqlite";
    std::ofstream(old_file, std::ios::binary) << "not a sqlite database";
    const auto original = FileBytes(old_file);
    auto [owner, opened] = PlayerData::Open(root, running);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(Read(*owner, "owner/model"_sv).result.outcome, Outcome::kMissing);
    PeerDatabase target(root / "preferences-c2.sqlite");
    EXPECT_EQ(target.Value("PRAGMA application_id"_sv), "1498631425");
    EXPECT_EQ(target.Value("PRAGMA user_version"_sv), "1");
    EXPECT_EQ(target.Value("SELECT compatibility_epoch FROM storage_meta WHERE id=1"_sv), "2");
    EXPECT_EQ(target.Value("SELECT count(*) FROM preferences"_sv), "0");
    EXPECT_EQ(FileBytes(old_file), original);
}

TEST_F(StorageTest, InitializationCancellationAndDiskFullRollbackAllState) {
    Bootstrap();
    FaultVfs faults;
    CancellationState cancelled;
    faults.before_player_open = [&] { cancelled.Request(); };
    auto [owner, result] = PlayerData::Open(root, cancelled);
    EXPECT_TRUE(cancelled.IsRequested());
    EXPECT_EQ(owner, nullptr);
    EXPECT_EQ(result.failure, Failure::kCancelled);
    {
        PeerDatabase target(root / "preferences-c2.sqlite");
        EXPECT_EQ(target.Value("SELECT count(*) FROM sqlite_schema"_sv), "0");
        EXPECT_EQ(target.Value("PRAGMA application_id"_sv), "0");
        EXPECT_EQ(target.Value("PRAGMA user_version"_sv), "0");
    }
    auto [residue, rejection] = PlayerData::Open(root, running);
    EXPECT_EQ(residue, nullptr);
    EXPECT_EQ(rejection.failure, Failure::kIncompatible);
    std::filesystem::remove(root / "preferences-c2.sqlite");
    faults.disk_full = true;
    auto [full, failure] = PlayerData::Open(root, running);
    EXPECT_EQ(full, nullptr);
    EXPECT_EQ(failure.failure, Failure::kAccessIo);
    faults.disk_full = false;
    PeerDatabase target(root / "preferences-c2.sqlite");
    EXPECT_EQ(target.Value("SELECT count(*) FROM sqlite_schema"_sv), "0");
    EXPECT_EQ(target.Value("PRAGMA application_id"_sv), "0");
    EXPECT_EQ(target.Value("PRAGMA user_version"_sv), "0");
}

TEST_F(StorageTest, EpochRoleMetadataRevisionAndKnownLayoutAreValidatedBeforePublishingOwner) {
    constexpr std::array changes{"UPDATE storage_meta SET compatibility_epoch=3"_sv, "DELETE FROM storage_meta"_sv,
        "PRAGMA user_version=0"_sv, "PRAGMA application_id=0"_sv, "ALTER TABLE preferences RENAME COLUMN value TO unknown_value"_sv};
    for (std::size_t index = 0; index < changes.size(); ++index) {
        const auto scope = root / std::to_string(index);
        std::filesystem::create_directory(scope);
        auto [owner, opened] = PlayerData::Open(scope, running);
        ASSERT_NE(owner, nullptr);
        owner.reset();
        { PeerDatabase target(scope / "preferences-c2.sqlite"); target.Exec(changes[index]); }
        const auto original = FileBytes(scope / "preferences-c2.sqlite");
        auto [rejected, result] = PlayerData::Open(scope, running);
        EXPECT_EQ(rejected, nullptr);
        EXPECT_EQ(result.failure, Failure::kIncompatible);
        EXPECT_EQ(FileBytes(scope / "preferences-c2.sqlite"), original);
    }
}

TEST_F(StorageTest, OlderOpenConnectionSurvivesCompatiblePeerRevisionAndPreservesUnknownColumns) {
    auto [owner, opened] = PlayerData::Open(root, running);
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(Put(*owner, "owner/model"_sv, "before"_sv).result.committed);
    ASSERT_EQ(Read(*owner, "owner/model"_sv).values.size(), 1);
    PeerDatabase future(root / "preferences-c2.sqlite");
    future.Exec("BEGIN IMMEDIATE;ALTER TABLE preferences ADD COLUMN future TEXT NOT NULL DEFAULT 'new';"
        "CREATE TABLE extension(key TEXT PRIMARY KEY,value BLOB NOT NULL) STRICT;"
        "INSERT INTO extension(key,value) VALUES('independent',X'010203');"
        "UPDATE preferences SET future='preserve';PRAGMA user_version=7;COMMIT;"_sv);
    ASSERT_TRUE(Put(*owner, "owner/model"_sv, "after"_sv).result.committed);
    ASSERT_TRUE(Put(*owner, "owner/other"_sv, "new"_sv).result.committed);
    EXPECT_EQ(Read(*owner, "owner/model"_sv).values[0].value, "after");
    auto [reopened, result] = PlayerData::Open(root, running);
    ASSERT_NE(reopened, nullptr);
    ASSERT_TRUE(reopened->InsertIfAbsentAndRead(kPlayer, FactKind::kRoaming, "owner/model"_sv, "refused"_sv, 0, running).result.committed);
    EXPECT_EQ(Read(*reopened, "owner/model"_sv).values[0].value, "after");
    EXPECT_EQ(future.Value("SELECT future FROM preferences WHERE identity='owner/model'"_sv), "preserve");
    EXPECT_EQ(future.Value("SELECT future FROM preferences WHERE identity='owner/other'"_sv), "new");
    EXPECT_EQ(future.Value("SELECT hex(value) FROM extension WHERE key='independent'"_sv), "010203");
    EXPECT_EQ(future.Value("PRAGMA user_version"_sv), "7");
}

TEST_F(StorageTest, ConcurrentCreatorsRecheckCommittedWinnerAndNeverOverwriteItsFacts) {
    std::latch start(1);
    const auto open = [&](std::string_view value) {
        start.wait();
        auto [owner, result] = PlayerData::Open(root, running);
        if (owner) {
            const auto write = owner->InsertIfAbsentAndRead(kPlayer, FactKind::kRoaming, "winner"_sv, value, 9, running);
            EXPECT_TRUE(write.result.committed);
        } else {
            EXPECT_TRUE(result.failure == Failure::kResourceBusy || result.failure == Failure::kIncompatible);
        }
    };
    auto first = std::async(std::launch::async, [&] { open("first"_sv); });
    auto second = std::async(std::launch::async, [&] { open("second"_sv); });
    start.count_down(); first.get(); second.get();
    auto [owner, result] = PlayerData::Open(root, running);
    ASSERT_NE(owner, nullptr);
    const auto winner = Read(*owner, "winner"_sv);
    ASSERT_EQ(winner.values.size(), 1);
    EXPECT_TRUE(winner.values[0].value == "first" || winner.values[0].value == "second");
    const auto retained = owner->InsertIfAbsentAndRead(kPlayer, FactKind::kRoaming, "winner"_sv, "overwrite"_sv, 10, running);
    EXPECT_EQ(retained.values[0].value, winner.values[0].value);
    PeerDatabase target(root / "preferences-c2.sqlite");
    EXPECT_EQ(target.Value("SELECT count(*) FROM storage_meta"_sv), "1");
}

TEST_F(StorageTest, CreationQualificationDoesNotInitializeAnExistingZeroFileDuringAnotherOpen) {
    Bootstrap();
    FaultVfs faults;
    std::latch created(1), resume(1);
    faults.before_player_open = [&] { created.count_down(); resume.wait(); };
    auto creator = std::async(std::launch::async, [&] { return PlayerData::Open(root, running); });
    created.wait();
    auto [peer, rejected] = PlayerData::Open(root, running);
    EXPECT_EQ(peer, nullptr);
    EXPECT_EQ(rejected.failure, Failure::kIncompatible);
    EXPECT_EQ(std::filesystem::file_size(root / "preferences-c2.sqlite"), 0u);
    resume.count_down();
    auto [owner, opened] = creator.get();
    ASSERT_NE(owner, nullptr);
    EXPECT_TRUE(Put(*owner, "winner"_sv, "original"_sv).result.committed);
}

TEST_F(StorageTest, PrivateAllocatorAndAtomicFirstFill) {
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    EXPECT_EQ(sqlite3_libversion_number(), SQLITE_VERSION_NUMBER);
    EXPECT_GE(sqlite3_libversion_number(), 3053004);
    EXPECT_EQ(sqlite3_threadsafe(), 1);
    EXPECT_TRUE(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION"));
    EXPECT_TRUE(sqlite3_compileoption_used("ZERO_MALLOC"));
    EXPECT_TRUE(sqlite3_compileoption_used("DQS=0"));
    EXPECT_TRUE(sqlite3_compileoption_used("DEFAULT_MEMSTATUS=0"));
    auto allocation = sqlite3_malloc(32);
    ASSERT_NE(allocation, nullptr);
    EXPECT_TRUE(mi_is_in_heap_region(allocation));
    sqlite3_free(allocation);
    const auto first = database->InsertIfAbsentAndRead(kPlayer, FactKind::kRoaming, "owner/model@1.0.0"_sv, "first"_sv, 1, running);
    ASSERT_EQ(first.result.outcome, Outcome::kCommitted);
    const auto second = database->InsertIfAbsentAndRead(kPlayer, FactKind::kRoaming, "owner/model@1.0.0"_sv, "later"_sv, 2, running);
    ASSERT_EQ(second.values.size(), 1);
    EXPECT_EQ(second.values[0].value, "first");
}

TEST_F(StorageTest, LongScopePathsSupportWalAndContinuousConsumers) {
    auto scope = root;
    for (int index = 0; index < 5; ++index) scope /= std::string(50, 'a' + index);
    auto cleanup_root = root;
    auto native_scope = scope;
#if defined(YSM_WINDOWS)
    ASSERT_GT(scope.native().size(), 260u);
    cleanup_root = L"\\\\?\\" + root.native();
    native_scope = L"\\\\?\\" + scope.native();
#endif
    auto cleanup = ScopeGuard([&] { std::filesystem::remove_all(cleanup_root); });
    std::filesystem::create_directories(native_scope);
    auto [consumer, registered] = Coordinator::Open(scope, running);
    ASSERT_NE(consumer, nullptr);
    auto [database, opened] = PlayerData::Open(scope, running);
    ASSERT_NE(database, nullptr);
    EXPECT_EQ(Put(*database, "owner/model"_sv, "value"_sv).result.outcome, Outcome::kCommitted);
    ASSERT_EQ(consumer->BeginCleanup(running).outcome, Outcome::kFound);
    EXPECT_EQ(consumer->EndCleanup().outcome, Outcome::kCommitted);
}

TEST_F(StorageTest, ExistingEmptyForeignAndWrongRoleArePreserved) {
    const auto file = root / "preferences-c2.sqlite";
    std::ofstream(file).close();
    auto [rejected, failed] = PlayerData::Open(root, running);
    EXPECT_EQ(rejected, nullptr);
    EXPECT_EQ(failed.failure, Failure::kIncompatible);
    EXPECT_EQ(std::filesystem::file_size(file), 0);
    std::filesystem::remove(file);
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(Put(*database, "owner/model"_sv, "complete"_sv).result.committed);
    database.reset();
    std::filesystem::copy_file(file, root / "catalog.sqlite");
    auto [wrong, mismatch] = CacheCatalog::Open(root, running);
    EXPECT_EQ(wrong, nullptr);
    EXPECT_EQ(mismatch.failure, Failure::kIncompatible);
    auto [reopened, restored] = PlayerData::Open(root, running);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(Read(*reopened, "owner/model"_sv).values[0].value, "complete");
}

TEST_F(StorageTest, CancellationNeverMeansMissingAndConnectionRemainsUsable) {
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    CancellationState cancelled;
    cancelled.Request();
    const auto result = database->PutPreference(kPlayer, FactKind::kRoaming, "owner/model"_sv, "value"_sv, 1, cancelled);
    EXPECT_EQ(result.result.outcome, Outcome::kCancelled);
    EXPECT_FALSE(result.result.committed);
    EXPECT_EQ(Read(*database, "owner/model"_sv).result.outcome, Outcome::kMissing);
    EXPECT_EQ(Put(*database, "owner/model"_sv, "value"_sv).result.outcome, Outcome::kCommitted);
}

TEST_F(StorageTest, PeerRegistrationBlocksCleanupAndIndependentWalStillWrites) {
    auto [first, opened] = Coordinator::Open(root, running);
    ASSERT_NE(first, nullptr);
    auto [second, second_opened] = Coordinator::Open(root, running);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->BeginCleanup(running).outcome, Outcome::kRejected);
    auto [database, db_opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    EXPECT_EQ(Put(*database, "owner/model"_sv, "value"_sv).result.outcome, Outcome::kCommitted);
    second.reset();
    ASSERT_EQ(first->BeginCleanup(running).outcome, Outcome::kFound);
    auto [blocked, refused] = Coordinator::Open(root, running);
    EXPECT_EQ(blocked, nullptr);
    EXPECT_EQ(refused.failure, Failure::kResourceBusy);
    EXPECT_EQ(Put(*database, "owner/model"_sv, "next"_sv).result.outcome, Outcome::kCommitted);
    ASSERT_EQ(first->EndCleanup().outcome, Outcome::kCommitted);
    auto [third, third_opened] = Coordinator::Open(root, running);
    EXPECT_NE(third, nullptr);
}

TEST_F(StorageTest, RestoreFailureKeepsAdmissionUntilTheScopeIsDrainedAndClosed) {
    Bootstrap();
    FaultVfs faults;
    auto [owner, registered] = Coordinator::Open(root, running);
    ASSERT_NE(owner, nullptr);
    ASSERT_EQ(owner->BeginCleanup(running).outcome, Outcome::kFound);
    faults.refuse_shared = true;
    testing::internal::CaptureStderr();
    const auto failed = owner->EndCleanup();
    const auto diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(failed.failure, Failure::kAccessIo);
    EXPECT_NE(diagnostic.find("sqlite_extended=" + std::to_string(SQLITE_IOERR_LOCK)), std::string::npos);
    faults.refuse_shared = false;
    auto [blocked, rejection] = Coordinator::Open(root, running);
    EXPECT_EQ(blocked, nullptr);
    EXPECT_EQ(rejection.failure, Failure::kResourceBusy);
    owner.reset();
    auto [recovered, recovery] = Coordinator::Open(root, running);
    EXPECT_NE(recovered, nullptr);
}

TEST_F(StorageTest, DiskFullRollsBackTheWholeValueAndDoesNotBecomeMissing) {
    Bootstrap();
    FaultVfs faults;
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(Put(*database, "owner/model"_sv, "old-complete"_sv).result.committed);
    faults.disk_full = true;
    const auto failed = Put(*database, "owner/model"_sv, "new-complete"_sv);
    EXPECT_EQ(failed.result.failure, Failure::kAccessIo);
    EXPECT_FALSE(failed.result.committed);
    faults.disk_full = false;
    const auto old = Read(*database, "owner/model"_sv);
    ASSERT_EQ(old.result.outcome, Outcome::kFound);
    ASSERT_EQ(old.values.size(), 1);
    EXPECT_EQ(old.values[0].value, "old-complete");
    faults.read_only = true;
    const auto read_only = Put(*database, "owner/model"_sv, "refused"_sv);
    EXPECT_EQ(read_only.result.failure, Failure::kAccessIo);
    EXPECT_FALSE(read_only.result.committed);
    faults.read_only = false;
    EXPECT_EQ(Read(*database, "owner/model"_sv).values[0].value, "old-complete");
}

TEST_F(StorageTest, PlayerFailuresFollowStorageDebugLogConfigurationWithoutWarning) {
    Bootstrap();
    FaultVfs faults;
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(Put(*database, "owner/model"_sv, "old-complete"_sv).result.committed);
    faults.disk_full = true;
    auto restore_logging = ScopeGuard([] {
        static_cast<void>(ConfigureLogLevel(static_cast<int>(LogLevel::kDebug)));
    });
    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kInfo)).ok());
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const auto quiet = Put(*database, "owner/model"_sv, "refused"_sv);
    const auto quiet_output = testing::internal::GetCapturedStdout();
    const auto quiet_errors = testing::internal::GetCapturedStderr();
    EXPECT_EQ(quiet.result.failure, Failure::kAccessIo);
    EXPECT_TRUE(quiet_output.empty());
    EXPECT_TRUE(quiet_errors.empty());

    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kDebug)).ok());
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const auto diagnosed = Put(*database, "owner/model"_sv, "refused"_sv);
    const auto diagnostic = testing::internal::GetCapturedStdout();
    const auto errors = testing::internal::GetCapturedStderr();
    EXPECT_EQ(diagnosed.result.failure, Failure::kAccessIo);
    if constexpr (YSM_TEST_STORAGE_DEBUG_LOG) {
        EXPECT_NE(diagnostic.find("[YSM Native/DEBUG]"), std::string::npos);
        EXPECT_NE(diagnostic.find("sqlite_extended=" + std::to_string(SQLITE_FULL)), std::string::npos);
    } else {
        EXPECT_TRUE(diagnostic.empty());
    }
    EXPECT_TRUE(errors.empty());
}

TEST_F(StorageTest, RealLockCancellationAndBudgetDoNotPoisonReuse) {
    Bootstrap();
    FaultVfs faults;
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    const auto first = Put(*database, "key-a"_sv, "a-complete"_sv);
    ASSERT_TRUE(first.result.committed);
    const auto missing = Read(*database, "key-b"_sv);
    EXPECT_EQ(missing.result.outcome, Outcome::kMissing);
    EXPECT_TRUE(missing.values.empty());
    const auto third = Put(*database, "key-c"_sv, "c-complete"_sv);
    ASSERT_TRUE(third.result.committed);
    sqlite3* peer = nullptr;
    const auto file = (root / "preferences-c2.sqlite").string();
    ASSERT_EQ(sqlite3_open(file.c_str(), &peer), SQLITE_OK);
    auto close = ScopeGuard([&] { sqlite3_close(peer); });
    ASSERT_EQ(sqlite3_exec(peer, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    CancellationState stop;
    faults.request_on_busy = &stop;
    auto active = std::async(std::launch::async, [&] {
        return database->PutPreference(kPlayer, FactKind::kRoaming, "key-a"_sv, "cancelled"_sv, 2, stop);
    });
    ASSERT_EQ(active.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    const auto cancelled = active.get();
    faults.request_on_busy = nullptr;
    EXPECT_TRUE(stop.IsRequested());
    EXPECT_EQ(cancelled.result.outcome, Outcome::kCancelled);
    EXPECT_EQ(cancelled.result.failure, Failure::kCancelled);
    EXPECT_FALSE(cancelled.result.committed);
    EXPECT_TRUE(cancelled.values.empty());
    const auto wait_started = std::chrono::steady_clock::now();
    auto timed = std::async(std::launch::async, [&] { return Put(*database, "key-a"_sv, "timed"_sv); });
    ASSERT_EQ(timed.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(timed.get().result.failure, Failure::kResourceBusy);
    EXPECT_GE(std::chrono::steady_clock::now() - wait_started, std::chrono::milliseconds(200));
    ASSERT_EQ(sqlite3_exec(peer, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
    const auto read_a = Read(*database, "key-a"_sv);
    const auto read_c = Read(*database, "key-c"_sv);
    ASSERT_EQ(read_a.result.outcome, Outcome::kFound);
    ASSERT_EQ(read_c.result.outcome, Outcome::kFound);
    ASSERT_EQ(read_a.values.size(), 1);
    ASSERT_EQ(read_c.values.size(), 1);
    EXPECT_EQ(read_a.values[0].value, "a-complete");
    EXPECT_EQ(read_c.values[0].value, "c-complete");
    sqlite3_stmt* read = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(peer, "SELECT identity,value FROM preferences ORDER BY identity", -1, &read, nullptr), SQLITE_OK);
    auto finish = ScopeGuard([&] { sqlite3_finalize(read); });
    constexpr std::array expected{std::pair{"key-a"_sv, "a-complete"_sv}, std::pair{"key-c"_sv, "c-complete"_sv}};
    for (const auto& [identity, value] : expected) {
        ASSERT_EQ(sqlite3_step(read), SQLITE_ROW);
        EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(read, 0)), identity.c_str());
        const auto* bytes = static_cast<const char*>(sqlite3_column_blob(read, 1));
        EXPECT_EQ(std::string_view(bytes, static_cast<std::size_t>(sqlite3_column_bytes(read, 1))), std::string_view(value));
    }
    EXPECT_EQ(sqlite3_step(read), SQLITE_DONE);
    database.reset();
    ASSERT_EQ(first.values.size(), 1);
    ASSERT_EQ(third.values.size(), 1);
    EXPECT_EQ(first.values[0].value, "a-complete");
    EXPECT_EQ(third.values[0].value, "c-complete");
}

TEST_F(StorageTest, CancelledCleanupRestoresProtectionWithoutReusingItsToken) {
    auto [owner, opened] = Coordinator::Open(root, running);
    ASSERT_NE(owner, nullptr);
    CancellationState cancelled;
    cancelled.Request();
    EXPECT_EQ(owner->BeginCleanup(cancelled).outcome, Outcome::kCancelled);
    EXPECT_EQ(owner->BeginCleanup(running).outcome, Outcome::kFound);
    cancelled.Request();
    EXPECT_EQ(owner->EndCleanup().outcome, Outcome::kCommitted);
    auto [peer, result] = Coordinator::Open(root, running);
    ASSERT_NE(peer, nullptr);
    EXPECT_EQ(owner->BeginCleanup(running).outcome, Outcome::kRejected);
}

TEST_F(StorageTest, CancellationDuringCommitIoPreservesTheRealCommit) {
    Bootstrap();
    FaultVfs faults;
    CancellationState race;
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(Put(*database, "key-a"_sv, "old"_sv).result.committed);
    faults.request_on_write = &race;
    const auto committed = database->PutPreference(kPlayer, FactKind::kRoaming, "key-a"_sv, "new"_sv, 2, race);
    faults.request_on_write = nullptr;
    EXPECT_TRUE(race.IsRequested());
    EXPECT_EQ(committed.result.outcome, Outcome::kCommitted);
    EXPECT_TRUE(committed.result.committed);
    sqlite3* peer = nullptr;
    const auto file = (root / "preferences-c2.sqlite").string();
    ASSERT_EQ(sqlite3_open(file.c_str(), &peer), SQLITE_OK);
    auto close = ScopeGuard([&] { sqlite3_close(peer); });
    sqlite3_stmt* read = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(peer, "SELECT value FROM preferences WHERE identity='key-a'", -1, &read, nullptr), SQLITE_OK);
    auto finish = ScopeGuard([&] { sqlite3_finalize(read); });
    ASSERT_EQ(sqlite3_step(read), SQLITE_ROW);
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(read, 0));
    EXPECT_EQ(std::string_view(bytes, static_cast<std::size_t>(sqlite3_column_bytes(read, 0))), "new");
    EXPECT_EQ(Read(*database, "key-a"_sv).values[0].value, "new");
}

#if !defined(YSM_WINDOWS)
TEST_F(StorageTest, FailedPrivateLocationCannotResumeAgainstAReplacement) {
    auto [database, opened] = PlayerData::Open(root, running);
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(Put(*database, "owner/model"_sv, "original"_sv).result.committed);
    auto moved = root;
    moved += "-moved";
    std::filesystem::rename(root, moved);
    auto restore = ScopeGuard([&] {
        database.reset();
        std::filesystem::remove_all(root);
        std::filesystem::rename(moved, root);
    });
    std::filesystem::create_directory(root);
    EXPECT_EQ(Read(*database, "owner/model"_sv).result.failure, Failure::kAccessIo);
    std::ofstream(root / "preferences-c2.sqlite").close();
    EXPECT_EQ(Put(*database, "owner/model"_sv, "replacement"_sv).result.failure, Failure::kClosed);
    EXPECT_EQ(std::filesystem::file_size(root / "preferences-c2.sqlite"), 0);
}
#endif

TEST(StorageEpochProcessChild, ExitDuringInitialization) {
    const auto scope = std::getenv("YSM_STORAGE_TEST_EXIT_SCOPE");
    if (!scope) GTEST_SKIP();
    const auto bootstrap = std::filesystem::path(scope) / "bootstrap";
    std::filesystem::create_directory(bootstrap);
    auto [initialized, bootstrap_result] = PlayerData::Open(bootstrap, CancellationState{});
    ASSERT_NE(initialized, nullptr);
    initialized.reset();
    FaultVfs faults;
    faults.after_player_main_write = [] { std::_Exit(72); };
    auto [owner, result] = PlayerData::Open(scope, CancellationState{});
    FAIL() << "Expected the child to exit during initialization";
}

TEST(StorageEpochProcessChild, VerifyInterruptedInitialization) {
    const auto scope = std::getenv("YSM_STORAGE_TEST_EXIT_SCOPE");
    if (!scope) GTEST_SKIP();
    auto [owner, result] = PlayerData::Open(scope, CancellationState{});
    EXPECT_EQ(owner, nullptr);
    EXPECT_EQ(result.failure, Failure::kIncompatible);
}

#if defined(YSM_LINUX)
TEST(StorageProcessChild, RegisterAndWait) {
    const auto scope = std::getenv("YSM_STORAGE_TEST_SCOPE");
    if (!scope) GTEST_SKIP();
    auto [owner, result] = Coordinator::Open(scope, CancellationState{});
    std::ofstream(std::filesystem::path(scope) / "child.ready") << static_cast<int>(result.failure);
    if (owner) {
        while (!std::filesystem::exists(std::filesystem::path(scope) / "child.stop"))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

TEST_F(StorageTest, RealProcessConsumerCrashAndCleanupAdmission) {
    auto [owner, opened] = Coordinator::Open(root, running);
    ASSERT_NE(owner, nullptr);
    const auto spawn = [&] {
        std::filesystem::remove(root / "child.ready");
        setenv("YSM_STORAGE_TEST_SCOPE", root.c_str(), 1);
        pid_t pid{};
        std::array<char*, 3> args{const_cast<char*>("/proc/self/exe"),
            const_cast<char*>("--gtest_filter=StorageProcessChild.RegisterAndWait"), nullptr};
        EXPECT_EQ(posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, args.data(), environ), 0);
        unsetenv("YSM_STORAGE_TEST_SCOPE");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!std::filesystem::exists(root / "child.ready") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return pid;
    };
    auto child = spawn();
    ScopeGuard reap([&] {
        if (child > 0) {
            kill(child, SIGKILL);
            int ignored{};
            waitpid(child, &ignored, 0);
        }
    });
    ASSERT_TRUE(std::filesystem::exists(root / "child.ready"));
    EXPECT_EQ(owner->BeginCleanup(running).outcome, Outcome::kRejected);
    kill(child, SIGKILL);
    int status{};
    ASSERT_EQ(waitpid(child, &status, 0), child);
    child = 0;
    ASSERT_EQ(owner->BeginCleanup(running).outcome, Outcome::kFound);
    child = spawn();
    ASSERT_EQ(waitpid(child, &status, 0), child);
    child = 0;
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    int failure{};
    std::ifstream(root / "child.ready") >> failure;
    EXPECT_EQ(failure, static_cast<int>(Failure::kResourceBusy));
    EXPECT_EQ(owner->EndCleanup().outcome, Outcome::kCommitted);
}
#endif
}  // namespace
}  // namespace ysm::storage

namespace ysm::storage {
namespace {
constexpr CStringView kTokenA = "00000000-0000-0000-0000-000000000001"_sv;
constexpr CStringView kTokenB = "00000000-0000-0000-0000-000000000002"_sv;
CacheSource FlatSource(std::uint64_t id, std::string_view token = kTokenA, std::string source = "model.zip",
    std::string scope = "scope-a", std::string version = "schema-a") {
    return {std::move(source), "owner/model", "cid", ConvertedObject{{id, std::string(token)}, std::move(scope),
        std::move(version), "owner/model", "cid", true}, "hint", "manifest"};
}
BakeObject Baked(std::uint64_t id, std::string_view token = kTokenA) {
    return {{{id, std::string(token)}, "payload-metadata"}, {{BakeKind::kAnimation, "animation-schema", "cid", "binding", "GUI"},
        "actual-input", "supported-abi", "capability", "engine-package", "actual-compiler", "frozen-declarations"}};
}

TEST_F(StorageTest, FileAllocatorPreservesAllBitsAndRejectsExhaustion) {
    auto [owner, opened] = FileIdAllocator::Open(root, running);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->Allocate(running).values.front(), 1);
    PeerDatabase observer(root / "ids.sqlite");
    EXPECT_EQ(observer.Value("SELECT typeof(next_id) FROM allocator"_sv), "integer");
    observer.Exec("UPDATE allocator SET next_id=0"_sv);
    EXPECT_EQ(owner->Allocate(running).values.front(), 0);
    observer.Exec("UPDATE allocator SET next_id=9223372036854775807"_sv);
    EXPECT_EQ(owner->Allocate(running).values.front(), 0x7fffffffffffffffULL);
    EXPECT_EQ(owner->Allocate(running).values.front(), 0x8000000000000000ULL);
    observer.Exec("UPDATE allocator SET next_id=-1"_sv);
    EXPECT_EQ(owner->Allocate(running).values.front(), 0xffffffffffffffffULL);
    auto exhausted = owner->Allocate(running);
    EXPECT_EQ(exhausted.result.failure, Failure::kCapacity);
    EXPECT_FALSE(exhausted.result.committed);
    owner.reset();
    auto [reopened, result] = FileIdAllocator::Open(root, running);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->Allocate(running).result.failure, Failure::kCapacity);
    EXPECT_EQ(observer.Value("SELECT next_id||':'||exhausted FROM allocator"_sv), "-1:1");
}

TEST_F(StorageTest, FileAllocatorConcurrentConnectionsAndFailureNeverReuseCommittedId) {
    auto [owner, opened] = FileIdAllocator::Open(root, running);
    ASSERT_NE(owner, nullptr);
    PeerDatabase observer(root / "ids.sqlite");
    observer.Exec("CREATE TRIGGER refuse_ids BEFORE UPDATE ON allocator BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    EXPECT_FALSE(owner->Allocate(running).result.committed);
    EXPECT_EQ(observer.Value("SELECT next_id FROM allocator"_sv), "1");
    observer.Exec("DROP TRIGGER refuse_ids"_sv);
    std::vector<std::unique_ptr<FileIdAllocator>> connections;
    for (int i = 0; i < 4; ++i) {
        auto [next, result] = FileIdAllocator::Open(root, running);
        ASSERT_NE(next, nullptr); connections.push_back(std::move(next));
    }
    std::vector<std::future<std::vector<std::uint64_t>>> tasks;
    for (auto& connection : connections) {
        auto* borrowed = connection.get();
        tasks.push_back(std::async(std::launch::async, [&, borrowed] {
            std::vector<std::uint64_t> ids;
            for (int i = 0; i < 32; ++i) {
                auto allocated = borrowed->Allocate(running);
                EXPECT_TRUE(allocated.result.committed);
                if (!allocated.values.empty()) ids.push_back(allocated.values.front());
            }
            return ids;
        }));
    }
    std::vector<std::uint64_t> ids;
    for (auto& task : tasks) { const auto batch = task.get(); ids.insert(ids.end(), batch.begin(), batch.end()); }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 128);
    for (std::size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], i + 1);
    EXPECT_EQ(observer.Value("SELECT next_id FROM allocator"_sv), "129");
}

TEST_F(StorageTest, FlatCatalogKeepsSourcesAndObjectsSeparateWithConditionalRevoke) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    auto old = FlatSource(0x8000000000000000ULL);
    ASSERT_TRUE(catalog->PublishSource("scope-a"_sv, old, running).committed);
    auto current = FlatSource(2, kTokenB);
    ASSERT_TRUE(catalog->PublishSource("scope-a"_sv, current, running).committed);
    auto stale = old.converted->location;
    EXPECT_TRUE(catalog->RevokeConverted(stale, running).committed);
    auto source = catalog->LookupSource("scope-a"_sv, "model.zip"_sv, running);
    ASSERT_TRUE(source.values.front().converted.has_value());
    EXPECT_EQ(source.values.front().converted->location.file_id, 2);
    EXPECT_EQ(catalog->References("cid"_sv, running).values.front().state, ReferenceState::kReferenced);
    EXPECT_EQ(catalog->PageConverted(std::nullopt, running).values.size(), 2);
    EXPECT_EQ(catalog->PageConverted(2, running).values.front().location.file_id, old.converted->location.file_id);
    EXPECT_TRUE(catalog->ForgetConverted(stale, running).committed);
    EXPECT_EQ(catalog->ForgetConverted(current.converted->location, running).outcome, Outcome::kRejected);
    EXPECT_TRUE(catalog->RevokeConverted(current.converted->location, running).committed);
    source = catalog->LookupSource("scope-a"_sv, "model.zip"_sv, running);
    EXPECT_FALSE(source.values.front().converted.has_value());
    EXPECT_EQ(source.values.front().container, "cid");
    EXPECT_EQ(catalog->References("cid"_sv, running).values.front().state, ReferenceState::kReferenced);
    EXPECT_EQ(catalog->PublishSource("scope-a"_sv, current, running).outcome, Outcome::kRejected);
    EXPECT_TRUE(catalog->ForgetConverted(current.converted->location, running).committed);
    auto reused = FlatSource(2, kTokenA);
    EXPECT_TRUE(catalog->PublishSource("scope-a"_sv, reused, running).committed);
    EXPECT_EQ(catalog->RevokeConverted(current.converted->location, running).outcome, Outcome::kRejected);
    EXPECT_EQ(catalog->LookupSource("scope-a"_sv, "model.zip"_sv, running).values.front().converted->location.instance, kTokenA.c_str());
}

TEST_F(StorageTest, CatalogInitializesSourceLocationIndexAndOpensUnderWriteContention) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    const auto source = FlatSource(7);
    ASSERT_TRUE(catalog->PublishSource("scope-a"_sv, source, running).committed);
    catalog.reset();

    PeerDatabase observer(root / "catalog.sqlite");
    constexpr auto index_count = "SELECT count(*) FROM pragma_index_list('sources') "
        "WHERE name='source_location' AND partial=1"_sv;
    EXPECT_EQ(observer.Value(index_count), "1");

    observer.Exec("BEGIN IMMEDIATE"_sv);
    auto [reopened, reopened_result] = CacheCatalog::Open(root, running, true);
    ASSERT_EQ(reopened_result.outcome, Outcome::kFound);
    ASSERT_NE(reopened, nullptr);
    auto persisted = reopened->LookupSource("scope-a"_sv, source.source, running);
    ASSERT_EQ(persisted.values.size(), 1);
    ASSERT_TRUE(persisted.values.front().converted.has_value());
    EXPECT_EQ(persisted.values.front().converted->location, source.converted->location);
    observer.Exec("ROLLBACK"_sv);
}

TEST_F(StorageTest, ObjectPagesPreserveUnsignedOrderAcrossTheSignBit) {
    auto [catalog, catalog_opened] = CacheCatalog::Open(root, running);
    auto [bake, bake_opened] = BakeIndex::Open(root, false, running);
    ASSERT_NE(catalog, nullptr);
    ASSERT_NE(bake, nullptr);
    constexpr auto sign_bit = std::uint64_t{1} << 63;
    constexpr auto last_id = (std::numeric_limits<std::uint64_t>::max)();
    std::vector<std::uint64_t> ids;
    for (std::uint64_t id = 0; id < kMaximumRows - 2; ++id) ids.push_back(id);
    ids.insert(ids.end(), {sign_bit - 1, sign_bit, sign_bit + 1, last_id});
    for (auto id : ids) {
        const std::string scope = id == sign_bit ? "scope-b" : "scope-a";
        auto source = FlatSource(id, kTokenA, std::to_string(id), scope);
        ASSERT_TRUE(catalog->PublishSource(scope, source, running).committed);
        ASSERT_TRUE(bake->Publish(Baked(id), running).committed);
    }

    const auto converted = catalog->PageConverted(std::nullopt, running);
    const auto baked = bake->PageObjects(std::nullopt, running);
    ASSERT_EQ(converted.values.size(), kMaximumRows);
    ASSERT_EQ(baked.values.size(), kMaximumRows);
    for (std::size_t index = 0; index < kMaximumRows; ++index) {
        EXPECT_EQ(converted.values[index].location.file_id, ids[index]);
        EXPECT_EQ(baked.values[index].file.location.file_id, ids[index]);
    }

    const auto scoped = catalog->PageConvertedScope("scope-a"_sv, std::nullopt, running);
    ASSERT_EQ(scoped.values.size(), kMaximumRows);
    EXPECT_EQ(scoped.values[kMaximumRows - 2].location.file_id, sign_bit - 1);
    EXPECT_EQ(scoped.values.back().location.file_id, sign_bit + 1);
    const auto next = catalog->PageConvertedScope("scope-a"_sv, sign_bit + 1, running);
    ASSERT_EQ(next.values.size(), 1);
    EXPECT_EQ(next.values.front().location.file_id, last_id);
    EXPECT_TRUE(catalog->PageConverted(last_id, running).values.empty());

    const auto negative = bake->PageObjects(sign_bit - 1, running);
    ASSERT_EQ(negative.values.size(), 3);
    EXPECT_EQ(negative.values.front().file.location.file_id, sign_bit);
    EXPECT_EQ(negative.values.back().file.location.file_id, last_id);
    EXPECT_TRUE(bake->PageObjects(last_id, running).values.empty());
}

TEST_F(StorageTest, FlatWritersPreserveCompatibleFutureColumns) {
    auto [catalog, catalog_opened] = CacheCatalog::Open(root, running);
    auto [remote, remote_opened] = CacheRemote::Open(root, running);
    auto [bake, bake_opened] = BakeIndex::Open(root, false, running);
    ASSERT_NE(catalog, nullptr); ASSERT_NE(remote, nullptr); ASSERT_NE(bake, nullptr);
    const CacheFile file{{7, std::string(kTokenA)}, "metadata"};
    auto write = [&] {
        EXPECT_TRUE(catalog->PublishSource("scope-a"_sv, FlatSource(8), running).committed);
        EXPECT_TRUE(catalog->PublishExact(ArtifactKind::kPreview, "preview"_sv, file, running).committed);
        EXPECT_TRUE(catalog->PublishDirectory("directory"_sv, "proof"_sv, running).committed);
        EXPECT_TRUE(catalog->SetScopeComplete("scope-a"_sv, true, running).committed);
        EXPECT_TRUE(catalog->SetReferencesComplete(true, running).committed);
        EXPECT_TRUE(catalog->ResetMaintenance(false, running).committed);
        EXPECT_TRUE(remote->PublishMetadata("metadata"_sv, file, "model"_sv, "cid"_sv, running).committed);
        EXPECT_TRUE(remote->MaxUsage("cid"_sv, 30, running).committed);
        EXPECT_TRUE(remote->SetReferencesComplete(true, running).committed);
    };
    write();
    ASSERT_TRUE(bake->Publish(Baked(9), running).committed);
    constexpr std::array catalog_tables{"objects"_sv, "sources"_sv, "exact"_sv, "directories"_sv, "scopes"_sv, "maintenance"_sv, "coverage"_sv};
    constexpr std::array remote_tables{"exact"_sv, "content_refs"_sv, "usage"_sv, "coverage"_sv};
    constexpr std::array bake_tables{"objects"_sv};
    PeerDatabase catalog_peer(root / "catalog.sqlite"), remote_peer(root / "remote.sqlite"), bake_peer(root / "bake.sqlite");
    auto extend = [](PeerDatabase& database, auto tables) {
        for (auto table : tables) {
            database.Exec(std::format("ALTER TABLE {} ADD COLUMN future TEXT NOT NULL DEFAULT 'new'", table));
            database.Exec(std::format("UPDATE {} SET future='preserved'", table));
        }
        database.Exec("PRAGMA user_version=2"_sv);
    };
    extend(catalog_peer, catalog_tables); extend(remote_peer, remote_tables); extend(bake_peer, bake_tables);
    write();
    ASSERT_TRUE(bake->Publish(Baked(10, kTokenB), running).committed);
    auto preserved = [](PeerDatabase& database, auto tables) {
        for (auto table : tables)
            EXPECT_EQ(database.Value(std::format("SELECT count(*) FROM {} WHERE future!='preserved'", table)), "0");
    };
    preserved(catalog_peer, catalog_tables); preserved(remote_peer, remote_tables);
    EXPECT_EQ(bake_peer.Value("SELECT future FROM objects WHERE file_id=9"_sv), "preserved");
    EXPECT_EQ(bake_peer.Value("SELECT future FROM objects WHERE file_id=10"_sv), "new");
    catalog.reset(); remote.reset(); bake.reset();
    EXPECT_NE(CacheCatalog::Open(root, running).first, nullptr);
    EXPECT_NE(CacheRemote::Open(root, running).first, nullptr);
    EXPECT_NE(BakeIndex::Open(root, false, running).first, nullptr);
}

TEST_F(StorageTest, DirectoryProofIsMetadataOnlyAndSurvivesCompatibleFutureColumns) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    constexpr CStringView proof = "directory-proof"_sv;
    ASSERT_TRUE(catalog->PublishDirectory("direct/cid"_sv, proof, running).committed);
    PeerDatabase observer(root / "catalog.sqlite");
    EXPECT_EQ(observer.Value("SELECT count(*) FROM objects"_sv), "0");
    EXPECT_EQ(observer.Value("SELECT count(*) FROM exact"_sv), "0");
    observer.Exec("ALTER TABLE directories ADD COLUMN future TEXT NOT NULL DEFAULT 'preserved'"_sv);
    observer.Exec("PRAGMA user_version=2"_sv);
    catalog.reset();
    auto [reopened, result] = CacheCatalog::Open(root, running);
    ASSERT_NE(reopened, nullptr);
    ASSERT_TRUE(reopened->PublishDirectory("direct/cid"_sv, "replacement"_sv, running).committed);
    EXPECT_EQ(reopened->LookupDirectory("direct/cid"_sv, running).values.front(), "replacement");
    EXPECT_EQ(observer.Value("SELECT future FROM directories"_sv), "preserved");
}

TEST_F(StorageTest, FlatCatalogPublicationRollsBackOwnershipAndPreservesOtherScopes) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    PeerDatabase observer(root / "catalog.sqlite");
    EXPECT_EQ(catalog->References("cid"_sv, running).values.front().state, ReferenceState::kUnknown);
    ASSERT_TRUE(catalog->SetReferencesComplete(true, running).committed);
    observer.Exec("CREATE TRIGGER refuse_source BEFORE INSERT ON sources BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    EXPECT_FALSE(catalog->PublishSource("scope-a"_sv, FlatSource(7), running).committed);
    EXPECT_EQ(observer.Value("SELECT count(*) FROM objects"_sv), "0");
    EXPECT_EQ(observer.Value("SELECT count(*) FROM scopes"_sv), "0");
    observer.Exec("DROP TRIGGER refuse_source"_sv);
    auto direct = FlatSource(0); direct.converted.reset();
    ASSERT_TRUE(catalog->PublishSource("scope-a"_sv, direct, running).committed);
    ASSERT_TRUE(catalog->SetScopeComplete("scope-a"_sv, true, running).committed);
    ASSERT_TRUE(catalog->SetScopeComplete("scope-unobserved"_sv, false, running).committed);
    auto refs = catalog->References("other"_sv, running).values.front();
    EXPECT_EQ(refs.state, ReferenceState::kUnknown); EXPECT_EQ(refs.domains, 3); EXPECT_EQ(refs.incomplete_domains, 1);
    ASSERT_TRUE(catalog->SetScopeComplete("scope-unobserved"_sv, true, running).committed);
    EXPECT_EQ(catalog->References("other"_sv, running).values.front().state, ReferenceState::kUnreferenced);
    auto other = FlatSource(9, kTokenB, "builtin", "scope-b");
    ASSERT_TRUE(catalog->PublishSource("scope-b"_sv, other, running).committed);
    const std::array obsolete{std::string("model.zip")};
    ASSERT_TRUE(catalog->FinishScan("scope-a"_sv, obsolete, running).committed);
    EXPECT_EQ(catalog->References("cid"_sv, running).values.front().state, ReferenceState::kReferenced);
    EXPECT_EQ(catalog->PageConverted(std::nullopt, running).values.size(), 1);
    ASSERT_TRUE(catalog->FinishScan("scope-b"_sv, std::array{std::string("builtin")}, running).committed);
    EXPECT_EQ(catalog->References("cid"_sv, running).values.front().state, ReferenceState::kUnknown);
    ASSERT_TRUE(catalog->SetScopeComplete("scope-b"_sv, true, running).committed);
    EXPECT_EQ(catalog->References("cid"_sv, running).values.front().state, ReferenceState::kUnreferenced);
    EXPECT_EQ(catalog->PageConverted(std::nullopt, running).values.size(), 1);
    EXPECT_EQ(observer.Value("SELECT count(*) FROM pragma_table_info('objects') WHERE name='path'"_sv), "0");
    EXPECT_EQ(observer.Value("SELECT typeof(file_id)||':'||cache_version FROM objects"_sv), "integer:schema-a");
    auto scopes = catalog->PageScopes(""_sv, running);
    EXPECT_EQ(scopes.values.size(), 3);
    EXPECT_TRUE(catalog->PageScopes(scopes.values.back().scope, running).values.empty());
    EXPECT_EQ(catalog->PageConvertedScope("scope-b"_sv, std::nullopt, running).values.size(), 1);
    EXPECT_TRUE(catalog->PageConvertedScope("scope-a"_sv, std::nullopt, running).values.empty());
}

TEST_F(StorageTest, InvalidCatalogRootCoverageDoesNotHideAnIncompleteScope) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    const std::array source{SourceReference{"direct"_sv, "model"_sv, "present"_sv}};
    ASSERT_TRUE(catalog->PublishSourceReferences("unobserved"_sv, source, running).committed);
    const std::array cids{std::string("present"), std::string("absent")};
    PeerDatabase observer(root / "catalog.sqlite");
    observer.Exec("PRAGMA ignore_check_constraints=ON"_sv);
    observer.Exec("UPDATE coverage SET complete=2 WHERE id=1"_sv);
    const auto corrupt = catalog->ReferencesMany(cids, running);
    EXPECT_EQ(corrupt.result.outcome, Outcome::kFailed);
    EXPECT_EQ(corrupt.result.failure, Failure::kContent);
    EXPECT_TRUE(corrupt.values.empty());

    ASSERT_TRUE(catalog->SetReferencesComplete(true, running).committed);
    const auto recovered = catalog->ReferencesMany(cids, running);
    ASSERT_EQ(recovered.values.size(), 2);
    EXPECT_EQ(recovered.values[0].state, ReferenceState::kReferenced);
    EXPECT_EQ(recovered.values[1].state, ReferenceState::kUnknown);
    EXPECT_EQ(recovered.values[1].domains, 2);
    EXPECT_EQ(recovered.values[1].incomplete_domains, 1);
}

TEST_F(StorageTest, SourceReferenceBatchesSkipUnchangedRowsAndRollBackOnFailure) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    const auto converted = FlatSource(7, kTokenA, "model.zip", "scope");
    ASSERT_TRUE(catalog->PublishSource("scope"_sv, converted, running).committed);
    const std::array first{SourceReference{converted.source, converted.model, converted.container}};
    ASSERT_TRUE(catalog->PublishSourceReferences("scope"_sv, first, running).committed);
    const auto direct = catalog->LookupSource("scope"_sv, converted.source, running);
    ASSERT_EQ(direct.values.size(), 1);
    EXPECT_FALSE(direct.values.front().converted);
    EXPECT_TRUE(direct.values.front().observation.empty());
    EXPECT_TRUE(direct.values.front().metadata.empty());
    EXPECT_EQ(catalog->PageConverted(std::nullopt, running).values.size(), 1);

    PeerDatabase observer(root / "catalog.sqlite");
    observer.Exec("CREATE TRIGGER refuse_update BEFORE UPDATE ON sources BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    EXPECT_TRUE(catalog->PublishSourceReferences("scope"_sv, first, running).committed);
    observer.Exec("CREATE TRIGGER refuse_source BEFORE INSERT ON sources WHEN NEW.source='late' "
        "BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    const std::array next{SourceReference{"early"_sv, "model"_sv, "cid"_sv},
        SourceReference{"late"_sv, "model"_sv, "cid"_sv}};
    EXPECT_FALSE(catalog->PublishSourceReferences("scope"_sv, next, running).committed);
    EXPECT_EQ(observer.Value("SELECT count(*) FROM sources"_sv), "1");
    EXPECT_EQ(catalog->LookupSource("scope"_sv, converted.source, running).result.outcome, Outcome::kFound);
}

TEST_F(StorageTest, FlatRemoteReferencesAndIntegerExactsAreIndependentOfUsage) {
    auto [remote, opened] = CacheRemote::Open(root, running);
    ASSERT_NE(remote, nullptr);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kUnknown);
    ASSERT_TRUE(remote->SetReferencesComplete(true, running).committed);
    CacheFile file{{0xffffffffffffffffULL, std::string(kTokenA)}, "bytes"};
    EXPECT_TRUE(remote->PublishExact(ArtifactKind::kMetadata, "cid"_sv, file, running).committed);
    EXPECT_EQ(remote->LookupExact(ArtifactKind::kMetadata, "cid"_sv, running).values.front().location.file_id, file.location.file_id);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kUnreferenced);
    EXPECT_TRUE(remote->MaxUsage("cid"_sv, 100, running).committed);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kUnreferenced);
    EXPECT_TRUE(remote->SetReferencesComplete(false, running).committed);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kUnknown);
    EXPECT_TRUE(remote->SetReference("accepted"_sv, "model"_sv, "cid"_sv, true, running).committed);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kReferenced);
    EXPECT_TRUE(remote->SetReference("accepted"_sv, "model"_sv, "different"_sv, false, running).committed);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kReferenced);
    EXPECT_TRUE(remote->SetReference("accepted"_sv, "model"_sv, "cid"_sv, false, running).committed);
    EXPECT_TRUE(remote->SetReferencesComplete(true, running).committed);
    EXPECT_EQ(remote->References("cid"_sv, running).values.front().state, ReferenceState::kUnreferenced);
}

TEST_F(StorageTest, RemoteMetadataAndSourceReferenceCommitOrRollbackTogether) {
    auto [remote, opened] = CacheRemote::Open(root, running);
    ASSERT_NE(remote, nullptr);
    ASSERT_TRUE(remote->SetReferencesComplete(true, running).committed);
    const CacheFile old{{1, std::string(kTokenA)}, "metadata"};
    ASSERT_TRUE(remote->PublishMetadata("source"_sv, old, "model-a"_sv, "cid-a"_sv, running).committed);
    PeerDatabase observer(root / "remote.sqlite");
    observer.Exec("CREATE TRIGGER refuse_reference BEFORE UPDATE ON content_refs BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    EXPECT_FALSE(remote->PublishMetadata("source"_sv, {{2, std::string(kTokenB)}, "new"}, "model-b"_sv, "cid-b"_sv, running).committed);
    EXPECT_EQ(remote->LookupExact(ArtifactKind::kMetadata, "source"_sv, running).values.front().location.file_id, 1);
    EXPECT_EQ(remote->References("cid-a"_sv, running).values.front().state, ReferenceState::kReferenced);
    EXPECT_EQ(remote->References("cid-b"_sv, running).values.front().state, ReferenceState::kUnreferenced);
}

TEST_F(StorageTest, BakeExactChecksEveryIdentityDimensionAndQueriesOnlyItsPartition) {
    auto [bake, opened] = BakeIndex::Open(root, false, running);
    ASSERT_NE(bake, nullptr);
    auto original = Baked(0xffffffffffffffffULL);
    ASSERT_TRUE(bake->Publish(original, running).committed);
    EXPECT_EQ(bake->LookupExact(original.key, running).values.front().location.file_id, original.file.location.file_id);
    for (int dimension = 0; dimension < 11; ++dimension) {
        auto key = original.key;
        switch (dimension) {
            case 0: key.partition.kind = BakeKind::kGeometry; break;
            case 1: key.partition.cache_version = "other-schema"; break;
            case 2: key.partition.container = "other-cid"; break;
            case 3: key.partition.binding = "other-binding"; break;
            case 4: key.partition.purpose = "FULL"; break;
            case 5: key.exact = "other-input"; break;
            case 6: key.abi = "other-abi"; break;
            case 7: key.capability = "other-capability"; break;
            case 8: key.engine_package = "other-engine-package"; break;
            case 9: key.compilation = "other-compiler"; break;
            case 10: key.frozen = "other-frozen"; break;
        }
        EXPECT_EQ(bake->LookupExact(key, running).result.outcome, Outcome::kMissing) << dimension;
    }
    auto partition = original.key.partition;
    EXPECT_TRUE(bake->HasPartition(partition, running).values.front());
    partition.purpose = "FULL";
    EXPECT_FALSE(bake->HasPartition(partition, running).values.front());
    auto other = Baked(0); other.key.partition.cache_version = "different-schema"; other.key.abi = "different-abi";
    ASSERT_TRUE(bake->Publish(other, running).committed);
    EXPECT_EQ(bake->PageObjects(std::nullopt, running).values.front().file.location.file_id, 0);
    EXPECT_EQ(bake->PageObjects(0, running).values.front().file.location.file_id, original.file.location.file_id);
    PeerDatabase observer(root / "bake.sqlite");
    EXPECT_EQ(observer.Value("SELECT count(*) FROM pragma_table_info('objects') WHERE name='path'"_sv), "0");
    EXPECT_EQ(observer.Value("SELECT typeof(file_id) FROM objects LIMIT 1"_sv), "integer");
    EXPECT_EQ(observer.Value("SELECT count(DISTINCT cache_version) FROM objects"_sv), "2");
}

TEST_F(StorageTest, BakePageDoesNotSkipTheFirstNegativeIdWhenItsByteBudgetIsReached) {
    auto [bake, opened] = BakeIndex::Open(root, false, running);
    ASSERT_NE(bake, nullptr);
    constexpr auto sign_bit = std::uint64_t{1} << 63;
    for (auto id : {sign_bit - 1, sign_bit}) {
        auto object = Baked(id);
        object.file.metadata.assign(kMaximumBytes / 2 + 1, 'x');
        ASSERT_TRUE(bake->Publish(object, running).committed);
    }

    const auto first = bake->PageObjects(std::nullopt, running);
    ASSERT_EQ(first.values.size(), 1);
    EXPECT_EQ(first.values.front().file.location.file_id, sign_bit - 1);
    const auto second = bake->PageObjects(first.values.front().file.location.file_id, running);
    ASSERT_EQ(second.values.size(), 1);
    EXPECT_EQ(second.values.front().file.location.file_id, sign_bit);
}

TEST_F(StorageTest, BakePublicationRollbackAndRetiredInstanceCannotAffectReplacement) {
    auto [bake, opened] = BakeIndex::Open(root, false, running);
    ASSERT_NE(bake, nullptr);
    auto old = Baked(1), current = Baked(2, kTokenB);
    ASSERT_TRUE(bake->Publish(old, running).committed);
    PeerDatabase observer(root / "bake.sqlite");
    observer.Exec("CREATE TRIGGER refuse_exact BEFORE INSERT ON objects BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    EXPECT_FALSE(bake->Publish(current, running).committed);
    EXPECT_EQ(observer.Value("SELECT count(*) FROM objects"_sv), "1");
    EXPECT_EQ(bake->LookupExact(old.key, running).values.front().location.file_id, 1);
    observer.Exec("DROP TRIGGER refuse_exact"_sv);
    ASSERT_TRUE(bake->Publish(current, running).committed);
    EXPECT_EQ(bake->PageObjects(std::nullopt, running).values.size(), 2);
    EXPECT_TRUE(bake->Revoke(old.file.location, running).committed);
    EXPECT_TRUE(bake->Forget(old.file.location, running).committed);
    auto reused = Baked(1, kTokenB);
    ASSERT_TRUE(bake->Publish(reused, running).committed);
    EXPECT_EQ(bake->Revoke(old.file.location, running).outcome, Outcome::kRejected);
    EXPECT_EQ(bake->LookupExact(old.key, running).values.front().location.instance, kTokenB.c_str());
    EXPECT_EQ(bake->Forget(reused.file.location, running).outcome, Outcome::kRejected);
}

TEST_F(StorageTest, BakeReadOnlyMissingDoesNotCreateAndExistingCannotWrite) {
    const auto before = std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{});
    auto [missing, absent] = BakeIndex::Open(root, true, running);
    EXPECT_EQ(missing, nullptr); EXPECT_EQ(absent.outcome, Outcome::kMissing);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), before);
    {
        auto [writer, result] = BakeIndex::Open(root, false, running);
        ASSERT_NE(writer, nullptr); ASSERT_TRUE(writer->Publish(Baked(0), running).committed);
    }
    const auto original = FileBytes(root / "bake.sqlite");
    {
        auto [reader, opened] = BakeIndex::Open(root, true, running);
        ASSERT_NE(reader, nullptr);
        EXPECT_TRUE(reader->HasPartition(Baked(0).key.partition, running).values.front());
        EXPECT_EQ(reader->Publish(Baked(1), running).failure, Failure::kAccessIo);
        EXPECT_EQ(reader->Revoke(Baked(0).file.location, running).failure, Failure::kAccessIo);
        EXPECT_EQ(reader->PageObjects(std::nullopt, running).values.size(), 1);
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
    }
    EXPECT_EQ(FileBytes(root / "bake.sqlite"), original);
}

TEST_F(StorageTest, MaintenanceResetAllAndConditionalCompletionAreAtomicAndPersistent) {
    auto [catalog, opened] = CacheCatalog::Open(root, running);
    ASSERT_NE(catalog, nullptr);
    auto state = catalog->ReadMaintenance(running).values.front();
    EXPECT_FALSE(state.last_success_utc.has_value()); EXPECT_EQ(state.reset_revision, 0); EXPECT_FALSE(state.pending_all);
    ASSERT_TRUE(catalog->CompleteMaintenance(0, 100, running).committed);
    ASSERT_TRUE(catalog->ResetMaintenance(true, running).committed);
    EXPECT_EQ(catalog->CompleteMaintenance(0, 200, running).outcome, Outcome::kRejected);
    state = catalog->ReadMaintenance(running).values.front();
    EXPECT_FALSE(state.last_success_utc.has_value()); EXPECT_EQ(state.reset_revision, 1); EXPECT_TRUE(state.pending_all);
    ASSERT_TRUE(catalog->ResetMaintenance(false, running).committed);
    EXPECT_TRUE(catalog->ReadMaintenance(running).values.front().pending_all);
    PeerDatabase observer(root / "catalog.sqlite");
    observer.Exec("CREATE TRIGGER refuse_maintenance BEFORE UPDATE ON maintenance BEGIN SELECT RAISE(ABORT,'injected'); END"_sv);
    EXPECT_FALSE(catalog->CompleteMaintenance(2, 300, running).committed);
    EXPECT_FALSE(catalog->ResetMaintenance(true, running).committed);
    state = catalog->ReadMaintenance(running).values.front();
    EXPECT_FALSE(state.last_success_utc.has_value()); EXPECT_EQ(state.reset_revision, 2); EXPECT_TRUE(state.pending_all);
    observer.Exec("DROP TRIGGER refuse_maintenance"_sv);
    ASSERT_TRUE(catalog->CompleteMaintenance(2, 300, running).committed);
    state = catalog->ReadMaintenance(running).values.front();
    EXPECT_EQ(state.last_success_utc, 300); EXPECT_FALSE(state.pending_all);
    catalog.reset();
    auto [reopened, result] = CacheCatalog::Open(root, running);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->ReadMaintenance(running).values.front().last_success_utc, 300);
    observer.Exec("UPDATE maintenance SET reset_revision=-1"_sv);
    EXPECT_EQ(reopened->ResetMaintenance(true, running).failure, Failure::kCapacity);
    EXPECT_EQ(observer.Value("SELECT reset_revision||':'||pending_all||':'||last_success FROM maintenance"_sv), "-1:0:300");
}

TEST_F(StorageTest, NewCacheRolesRejectEmptyWrongRoleAndIncompatibleWithoutTouchingOldLayout) {
    std::ofstream(root / "catalog.sqlite") << "old-catalog";
    auto [ids, opened] = FileIdAllocator::Open(root, running); ASSERT_NE(ids, nullptr); ids.reset();
    std::filesystem::copy_file(root / "ids.sqlite", root / "bake.sqlite");
    auto [wrong, incompatible] = BakeIndex::Open(root, false, running);
    EXPECT_EQ(wrong, nullptr); EXPECT_EQ(incompatible.failure, Failure::kIncompatible);
    EXPECT_EQ(FileBytes(root / "catalog.sqlite"), "old-catalog");
    std::ofstream(root / "catalog.sqlite");
    auto [empty, refused] = CacheCatalog::Open(root, running);
    EXPECT_EQ(empty, nullptr); EXPECT_EQ(refused.failure, Failure::kIncompatible);
    EXPECT_EQ(std::filesystem::file_size(root / "catalog.sqlite"), 0);
    PeerDatabase observer(root / "ids.sqlite"); observer.Exec("PRAGMA user_version=0"_sv);
    auto [old, rejected] = FileIdAllocator::Open(root, running);
    EXPECT_EQ(old, nullptr); EXPECT_EQ(rejected.failure, Failure::kIncompatible);
}
}  // namespace
}  // namespace ysm::storage
