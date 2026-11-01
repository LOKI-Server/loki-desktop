#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "store.hpp"

namespace loki {

struct Outcome {
    int downloaded = 0, uploaded = 0, removed = 0, conflicts = 0;
    std::vector<std::string> errors;
    std::string summary() const;
};

/**
 * Two-way sync of one server folder with one local directory.
 *
 * 1. Scan: files whose size or mtime moved since the last sync are hashed; files
 *    still being written (changed in the last few seconds) wait for the next pass.
 * 2. Pull: apply the server's change feed. A path changed on both sides keeps the
 *    local file and is pushed in step 3.
 * 3. Push: upload new content and commit it against the version last seen. Edits
 *    to synced files win with `overwrite` (the server keeps the other version as a
 *    conflict); files this computer never synced use `stash`, so a stale directory
 *    can never replace newer saves.
 *
 * Nothing is lost on either path: the server keeps every displaced version.
 * Downloads land in a temporary file, are checked against their hash, and then
 * replace the local file in one rename.
 */
class Engine {
public:
    explicit Engine(Store& store) : store_(store) {}

    /** Syncs one mapping and records the result on it. Never throws. */
    Outcome sync(const Mapping& mapping);
    std::map<std::string, Outcome> sync_all();

    /** Emulators write saves in bursts; wait until a file has been still this long. */
    static constexpr long long settle_ms = 3000;

    /** Tests only: runs just before a download replaces a local file. */
    std::function<void(const std::string& path)> before_replace;

private:
    Outcome sync_folder(const Mapping& mapping);
    Store& store_;
};

/** Names never synced: scratch files from this app and common OS clutter. */
bool ignored(const std::string& name);

}  // namespace loki
