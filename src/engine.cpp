#include "engine.hpp"

#include <chrono>
#include <mutex>
#include <random>
#include <set>

#include "index.hpp"
#include "sha256.hpp"

namespace fs = std::filesystem;

namespace loki {
namespace {

/** Only one sync runs at a time, whether started by the timer or the person. */
std::mutex sync_lock;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t mtime_ms(const fs::path& file) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(file));
    return std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
}

std::string logical(const fs::path& relative) {
    auto text = relative.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path physical(const fs::path& root, const std::string& path) {
    return root / fs::path(std::u8string(path.begin(), path.end()));
}

struct LocalFile {
    fs::path file;
    std::int64_t size;
    std::int64_t mtime;
};

std::map<std::string, LocalFile> scan(const fs::path& root) {
    std::map<std::string, LocalFile> files;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
    if (error) throw std::runtime_error("Cannot open " + root.string() + ": " + error.message());
    for (; it != end; it.increment(error)) {
        if (error) break;
        const auto& entry = *it;
        const auto name = logical(entry.path().filename());
        if (ignored(name)) {
            if (entry.is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (entry.is_symlink() || !entry.is_regular_file()) continue;
        files[logical(fs::relative(entry.path(), root))] = {entry.path(), std::int64_t(entry.file_size()),
                                                           mtime_ms(entry.path())};
    }
    return files;
}

std::string temporary_name() {
    static thread_local std::mt19937_64 random{std::random_device{}()};
    return ".loki-tmp-" + std::to_string(random());
}

struct Run {
    Api& api;
    fs::path root;
    SyncIndex& index;
    Outcome outcome;

    /** Writes a server version locally, all or nothing. */
    void download(const RemoteChange& change) {
        if (!change.hash) return;
        const auto target = physical(root, change.path);
        fs::create_directories(target.parent_path());
        const auto staged = target.parent_path() / temporary_name();
        try {
            api.download_blob(*change.hash, staged);
            if (Sha256::of_file(staged) != *change.hash) {
                throw std::runtime_error(change.path + " arrived damaged; it will be fetched again.");
            }
            fs::rename(staged, target);
        } catch (...) {
            std::error_code ignored_error;
            fs::remove(staged, ignored_error);
            throw;
        }
        index.entries[change.path] = {change.version, change.hash, std::int64_t(fs::file_size(target)),
                                      mtime_ms(target)};
        ++outcome.downloaded;
    }
};

}  // namespace

bool ignored(const std::string& name) {
    return name.rfind(".loki", 0) == 0 || name == "desktop.ini" || name == ".DS_Store" || name == "Thumbs.db";
}

std::string Outcome::summary() const {
    std::vector<std::string> parts;
    if (downloaded) parts.push_back(std::to_string(downloaded) + " received");
    if (uploaded) parts.push_back(std::to_string(uploaded) + " sent");
    if (removed) parts.push_back(std::to_string(removed) + " removed");
    if (conflicts) {
        parts.push_back(std::to_string(conflicts) + (conflicts == 1 ? " conflict" : " conflicts") + " kept on the server");
    }
    if (!errors.empty()) parts.push_back(std::to_string(errors.size()) + " skipped: " + errors.front());
    if (parts.empty()) return "Up to date";
    std::string text = parts.front();
    for (size_t i = 1; i < parts.size(); ++i) text += " \xC2\xB7 " + parts[i];
    return text;
}

std::map<std::string, Outcome> Engine::sync_all() {
    std::map<std::string, Outcome> outcomes;
    for (const auto& [id, mapping] : store_.mappings()) outcomes[id] = sync(mapping);
    return outcomes;
}

Outcome Engine::sync(const Mapping& mapping) {
    std::lock_guard lock(sync_lock);
    Outcome outcome;
    auto recorded = mapping;
    try {
        outcome = sync_folder(mapping);
        recorded.last_result = outcome.summary();
        recorded.failed = !outcome.errors.empty();
    } catch (const ApiError& e) {
        recorded.last_result = e.code == "unknown_folder" ? "This folder was deleted on the server."
                               : e.code == "unauthorized" ? "This computer was signed out. Pair it again."
                                                          : e.what();
        recorded.failed = true;
        outcome.errors.push_back(recorded.last_result);
    } catch (const std::exception& e) {
        recorded.last_result = e.what();
        recorded.failed = true;
        outcome.errors.push_back(recorded.last_result);
    }
    recorded.last_sync = now_ms();
    // The person may have stopped syncing this folder meanwhile.
    if (store_.mapping(mapping.folder_id)) store_.save_mapping(recorded);
    return outcome;
}

Outcome Engine::sync_folder(const Mapping& mapping) {
    auto api = store_.api();
    const auto root = mapping.local;
    if (!fs::is_directory(root)) throw std::runtime_error("The local folder " + root.string() + " is missing.");
    std::int64_t max_file = INT64_MAX;
    try {
        max_file = api.quota().max_file_bytes;
    } catch (const ApiError&) {
    }
    const auto index_file = store_.index_file(mapping.folder_id);
    auto index = SyncIndex::load(index_file);
    Run run{api, root, index, {}};

    // 1. Scan.
    auto local = scan(root);
    const auto now = now_ms();
    std::map<std::string, std::string> hashes;
    std::set<std::string> dirty, unsettled;
    for (const auto& [path, file] : local) {
        auto known = index.entries.find(path);
        if (known != index.entries.end() && known->second.hash && known->second.size == file.size &&
            known->second.mtime == file.mtime) {
            continue;
        }
        if (now - file.mtime < settle_ms) {
            unsettled.insert(path);
            continue;
        }
        const auto hash = Sha256::of_file(file.file);
        hashes[path] = hash;
        if (known != index.entries.end() && known->second.hash == hash) {
            known->second.size = file.size;
            known->second.mtime = file.mtime;
        } else {
            dirty.insert(path);
        }
    }
    std::set<std::string> deleted;
    for (const auto& [path, e] : index.entries) {
        if (e.hash && !local.contains(path)) deleted.insert(path);
    }

    // 2. Pull.
    auto since = index.cursor;
    for (;;) {
        auto feed = api.changes(mapping.folder_id, since);
        if (feed.reset) {
            // The server was restored: versions are unknown, contents still compare.
            for (auto& [path, e] : index.entries) e.version = 0;
        }
        for (const auto& change : feed.changes) {
            const auto& path = change.path;
            auto known = index.entries.find(path);
            if ((known != index.entries.end() && known->second.version == change.version) || unsettled.contains(path)) {
                continue;
            }
            if (dirty.contains(path)) {
                if (change.hash && hashes[path] == *change.hash) {
                    const auto& file = local.at(path);
                    index.entries[path] = {change.version, change.hash, file.size, file.mtime};
                    dirty.erase(path);
                }
            } else if (deleted.contains(path)) {
                deleted.erase(path);
                if (change.hash) run.download(change);
                else index.entries[path] = {change.version, std::nullopt, 0, 0};
            } else if (!change.hash) {
                if (local.contains(path)) {
                    fs::remove(local.at(path).file);
                    local.erase(path);
                    ++run.outcome.removed;
                }
                index.entries[path] = {change.version, std::nullopt, 0, 0};
            } else if (local.contains(path) && known != index.entries.end() && known->second.hash == change.hash) {
                known->second.version = change.version;
            } else {
                run.download(change);
            }
        }
        since = feed.seq;
        index.cursor = since;
        index.save(index_file);
        if (!feed.more) break;
    }
    local = scan(root);

    // 3. Push.
    std::vector<LocalChange> overwrite, stash, deletes;
    for (const auto& path : dirty) {
        auto file = local.find(path);
        if (file == local.end()) continue;
        if (file->second.size > max_file) {
            run.outcome.errors.push_back(path + " is larger than this server allows");
            continue;
        }
        const auto& hash = hashes.at(path);
        try {
            if (!api.has_blob(hash)) api.upload_blob(hash, file->second.file);
        } catch (const ApiError& e) {
            // Changed while uploading, too large, or over quota: try again next pass.
            run.outcome.errors.push_back(path + ": " + e.what());
            continue;
        }
        auto known = index.entries.find(path);
        LocalChange change{path, known != index.entries.end() ? known->second.version : 0, hash, file->second.size,
                           file->second.mtime / 1000};
        (known != index.entries.end() && known->second.hash ? overwrite : stash).push_back(change);
    }
    for (const auto& path : deleted) deletes.push_back({path, index.entries.at(path).version, std::nullopt, 0, now / 1000});

    const std::pair<std::vector<LocalChange>*, const char*> groups[] = {
        {&overwrite, "overwrite"}, {&stash, "stash"}, {&deletes, "reject"}};
    for (const auto& [changes, mode] : groups) {
        for (size_t start = 0; start < changes->size(); start += 500) {
            std::vector<LocalChange> batch(changes->begin() + start,
                                           changes->begin() + std::min(changes->size(), start + 500));
            std::map<std::string, LocalChange> sent;
            for (const auto& c : batch) sent[c.path] = c;
            for (const auto& result : api.commit(mapping.folder_id, batch, mode)) {
                const auto& change = sent.at(result.path);
                switch (result.status) {
                    case CommitResult::Status::Ok:
                        if (change.hash) {
                            const auto& file = local.at(result.path);
                            index.entries[result.path] = {result.version, change.hash, file.size, file.mtime};
                            // A version more than one ahead displaced someone else's edit.
                            if (result.version > change.base_version + 1) ++run.outcome.conflicts;
                        } else {
                            index.entries[result.path] = {result.version, std::nullopt, 0, 0};
                        }
                        ++run.outcome.uploaded;
                        break;
                    case CommitResult::Status::Stashed:
                        ++run.outcome.conflicts;
                        if (result.current && result.current->hash) run.download(*result.current);
                        break;
                    case CommitResult::Status::Conflict:
                        // A deletion raced a newer edit; the next pull brings the edit back.
                        break;
                    case CommitResult::Status::Rejected:
                        run.outcome.errors.push_back(result.path + ": " + result.error);
                        break;
                }
            }
            index.save(index_file);
        }
    }
    index.save(index_file);
    return run.outcome;
}

}  // namespace loki
