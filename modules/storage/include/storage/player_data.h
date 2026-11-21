#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <cancellation.h>
#include <pimpl.h>
#include <storage/result.h>

namespace ysm::storage {

enum class FactKind : std::int32_t { kRoaming, kFavourite, kLocalSelection };
struct PlayerFact {
    std::string identity;
    std::string value;
    std::int64_t updated_utc_seconds = 0;
};

// The Java owner serializes each complete operation and destruction. Results own
// complete values and do not borrow the business object's execution resources.
class PlayerData final {
public:
    static std::pair<std::unique_ptr<PlayerData>, Result> Open(
        const std::filesystem::path& root, const CancellationState& cancelled);
    ~PlayerData();
    Values<PlayerFact> ReadPreference(std::string_view player, FactKind kind,
        std::string_view identity, const CancellationState& cancelled);
    Values<PlayerFact> PutPreference(std::string_view player, FactKind kind,
        std::string_view identity, std::string_view value,
        std::int64_t utc_seconds, const CancellationState& cancelled);
    // A competing first fill returns the existing complete target.
    Values<PlayerFact> InsertIfAbsentAndRead(std::string_view player, FactKind kind,
        std::string_view identity, std::string_view value,
        std::int64_t utc_seconds, const CancellationState& cancelled);
    // Each page observes current facts; no cross-page snapshot is promised.
    Values<PlayerFact> PagePreferences(std::string_view player, FactKind kind,
        std::string_view after_identity, const CancellationState& cancelled);
    PlayerData(const PlayerData&) = delete;
    PlayerData& operator=(const PlayerData&) = delete;
private:
    PlayerData();
    YSM_PIMPL_DECLARE(Impl, sizeof(void*))
};

}  // namespace ysm::storage
