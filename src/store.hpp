#pragma once
// The signed-in account and folder mappings, kept as JSON in the user's config folder.
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "api.hpp"

namespace loki {

/** Where a server folder lives on this computer, and how its last sync went. */
struct Mapping {
    std::string folder_id;
    std::filesystem::path local;
    std::int64_t last_sync = 0;  // Unix milliseconds
    std::string last_result;
    bool failed = false;
};

class Store {
public:
    /** Loads (or starts) the configuration kept in `directory`. */
    explicit Store(std::filesystem::path directory);

    std::string server() const;
    std::string token() const;
    std::string display_name() const;
    bool signed_in() const;
    Api api() const;

    void sign_in(const std::string& server, const Session& session);
    void sign_out();

    std::map<std::string, Mapping> mappings() const;
    std::optional<Mapping> mapping(const std::string& folder_id) const;
    void save_mapping(const Mapping& mapping);
    void remove_mapping(const std::string& folder_id);

    std::filesystem::path index_file(const std::string& folder_id) const;
    const std::filesystem::path& directory() const { return directory_; }

private:
    void save() const;

    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::string server_, token_, username_, display_name_;
    std::map<std::string, Mapping> mappings_;
};

/** Writes `text` to `file` through a temporary file, so a crash never leaves half a file. */
void write_atomically(const std::filesystem::path& file, const std::string& text);
std::string read_text(const std::filesystem::path& file);

}  // namespace loki
