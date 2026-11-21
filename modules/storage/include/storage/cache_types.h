#pragma once

#include <cstdint>
#include <string>

namespace ysm::storage {

// IDs carry uint64 bit patterns through SQLite INTEGER and JNI long. They are
// addresses only; the independent instance token qualifies a production.
struct CacheLocation {
    std::uint64_t file_id = 0;
    std::string instance;
    bool operator==(const CacheLocation&) const = default;
};
struct CacheFile {
    CacheLocation location;
    std::string metadata;
};

enum class ArtifactKind : std::int32_t { kMetadata = 2, kChunk = 3, kPreview = 4 };

enum class ReferenceState : std::int32_t { kReferenced, kUnreferenced, kUnknown };
struct ContentReferences {
    ReferenceState state;
    std::int64_t domains = 0;
    std::int64_t incomplete_domains = 0;
};

}  // namespace ysm::storage
