#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace loki {

/**
 * What a path looked like the last time it was in step with the server: the server
 * version and hash (empty for a known deletion), plus the local size and mtime that
 * let unchanged files skip rehashing.
 */
struct Entry {
    std::int64_t version = 0;
    std::optional<std::string> hash;
    std::int64_t size = 0;
    std::int64_t mtime = 0;  // Unix milliseconds
};

/** The per-folder record of the last agreed state and the change-feed cursor. */
struct SyncIndex {
    std::int64_t cursor = 0;
    std::map<std::string, Entry> entries;

    void save(const std::filesystem::path& file) const;
    static SyncIndex load(const std::filesystem::path& file);
};

}  // namespace loki
