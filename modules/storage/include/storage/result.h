#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ysm::storage {

enum class Outcome : std::int32_t {
    kFound, kMissing, kCommitted, kRejected, kFailed, kCancelled
};
enum class Failure : std::int32_t {
    kNone, kResourceBusy, kCapacity, kAccessIo, kIncompatible, kContent,
    kClosed, kCancelled, kInternal
};
struct Result {
    Outcome outcome = Outcome::kFailed;
    Failure failure = Failure::kNone;
    bool committed = false;
};
template <typename T>
struct Values {
    Result result;
    std::vector<T> values;
};
constexpr std::size_t kMaximumBytes = 1024 * 1024;
constexpr std::size_t kMaximumRows = 64;

}  // namespace ysm::storage
