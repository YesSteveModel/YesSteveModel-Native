#pragma once

#include <filesystem>
#include <memory>
#include <utility>

#include <cancellation.h>
#include <pimpl.h>
#include <storage/result.h>

namespace ysm::storage {

// One registration per process/scope. EndCleanup restores consumer protection
// before releasing admission. Failed restoration retains admission until Close;
// the owner may close only after the last file consumer has ended. BeginCleanup
// returns kRejected for admission contention or peer consumers only while this
// registration still holds SHARED protection.
class Coordinator final {
public:
    static std::pair<std::unique_ptr<Coordinator>, Result> Open(
        const std::filesystem::path& scope, const CancellationState& cancelled);
    ~Coordinator();
    Result BeginCleanup(const CancellationState& cancelled);
    Result EndCleanup();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
private:
    Coordinator();
    YSM_PIMPL_DECLARE(Impl, 12 * sizeof(void*))
};

}  // namespace ysm::storage
