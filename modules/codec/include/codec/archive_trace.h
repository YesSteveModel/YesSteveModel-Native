#pragma once

#include <cstdint>
#include <vector>

namespace ysm::codec {
// Optional per-owner observation. The caller keeps this sink alive through archive destruction.
struct ArchiveTrace {
    struct Read {
        uint64_t offset;
        uint64_t size;
    };

    std::vector<Read> reads;
    uint64_t directories = 0;
    uint64_t extracts = 0;
};
}  // namespace ysm::codec
