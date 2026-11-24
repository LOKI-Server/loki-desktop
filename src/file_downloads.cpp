#include "file_downloads.hpp"

#include "desktop.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <random>
#include <unordered_map>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace loki {
namespace {

enum class Kind { missing, file, directory, other };
constexpr auto stage_prefix = ".loki-transfer-";

fs::path io_path(const fs::path& path) {
#ifdef _WIN32
    const auto native = fs::path(path).make_preferred();
    const auto& text = native.native();
    if (path.is_absolute() && !text.starts_with(L"\\\\?\\")) {
        if (text.starts_with(L"\\\\"))
            return L"\\\\?\\UNC\\" + text.substr(2);
        return L"\\\\?\\" + text;
    }
#endif
    return path;
}

Kind kind(const fs::path& path) {
    const auto native = io_path(path);
    const auto status = fs::symlink_status(native);
    if (!fs::exists(status)) return Kind::missing;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(native.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Kind::other;
#endif
    if (fs::is_regular_file(status)) return Kind::file;
    if (fs::is_directory(status)) return Kind::directory;
    return Kind::other;
}

fs::path destination_path(const fs::path& path) {
    if (!path.is_absolute() || !path.has_filename()) throw std::runtime_error("Choose a download location.");
    if (path.filename() == "." || path.filename() == "..") throw std::runtime_error("Choose a named file or folder.");
    const auto parent = fs::canonical(path.parent_path());
    if (!fs::is_directory(io_path(parent))) throw std::runtime_error("Choose an existing destination folder.");
    return parent / path.filename();
}

std::string remote_member(const std::string& root, const std::string& name) {
    return name.empty() ? root : root.empty() ? name : root + "/" + name;
}

fs::path local_member(const fs::path& root, const std::string& name) {
    return name.empty() ? root : root / safe_download_path(name);
}

std::string random_id() {
    std::random_device random;
    std::array<unsigned int, 8> bytes;
    for (auto& value : bytes) value = random();
    return Sha256::of(std::string_view(reinterpret_cast<const char*>(bytes.data()), sizeof(bytes)));
}

struct Stage {
    fs::path path;
    bool preserve = false;
    explicit Stage(const fs::path& target) {
        path = target.parent_path() / (stage_prefix + random_id());
        if (!fs::create_directory(io_path(path))) throw std::runtime_error("Could not create a temporary download folder.");
    }
    ~Stage() {
        if (preserve) return;
        std::error_code ignored;
        fs::remove_all(io_path(path), ignored);
    }
};

bool regular_matches(const fs::path& path, const FileMetadata& file) {
    return kind(path) == Kind::file && fs::file_size(io_path(path)) == std::uint64_t(file.size)
           && Sha256::of_file(io_path(path)) == file.sha256;
}

void require_active(const DownloadProgress& progress) {
    if (!progress(0, 0)) throw std::runtime_error("Download cancelled.");
}

fs::path record_path(const fs::path& registry, const std::string& id) {
    if (!valid_file_hash(id)) throw std::runtime_error("Invalid tracked download ID.");
    return registry / (id + ".json");
}

json encode(const TrackedDownload& item) {
    json entries = json::array();
    for (const auto& [name, entry] : item.baseline.entries) {
        entries.push_back({{"path", name}, {"directory", entry.directory}, {"size", entry.file.size}, {"sha256", entry.file.sha256}});
    }
    return {{"share", item.share}, {"share_name", item.share_name}, {"remote_path", item.remote_path},
            {"destination", path_utf8(item.destination)}, {"directory", item.baseline.directory},
            {"revision", item.baseline.revision}, {"entries", std::move(entries)}};
}

void recover(const json& pending) {
    const auto target = path_from_utf8(pending.at("target"));
    const auto stage = path_from_utf8(pending.at("stage"));
    const auto name = path_utf8(stage.filename());
    if (!target.is_absolute() || stage.parent_path() != target.parent_path() || stage == target ||
        !name.starts_with(stage_prefix) || !valid_file_hash(name.substr(std::string_view(stage_prefix).size())) ||
        kind(stage) != Kind::directory)
        throw std::runtime_error("The interrupted download needs recovery before it can be updated.");
    const auto old = io_path(stage / "old");
    const auto fresh = io_path(stage / "new");
    if (kind(old) != Kind::missing) {
        if (kind(target) != Kind::missing) {
            if (kind(fresh) != Kind::missing)
                throw std::runtime_error("The destination changed during recovery. Previous files are in " + path_utf8(old));
            fs::rename(io_path(target), fresh);
        }
        fs::rename(old, io_path(target));
    } else if (!pending.at("had_target").get<bool>() && kind(fresh) == Kind::missing && kind(target) != Kind::missing) {
        fs::rename(io_path(target), fresh);
    }
}

std::optional<TrackedDownload> find(const fs::path& registry, const std::string& id) {
    const auto record = record_path(registry, id);
    if (!fs::exists(record)) return std::nullopt;
    auto data = json::parse(read_text(record));
    if (data.contains("pending")) {
        const auto pending = data.at("pending");
        recover(pending);
        const bool discard = pending.value("discard_record", false);
        data.erase("pending");
        if (discard) fs::remove(record);
        else write_atomically(record, data.dump());
        std::error_code ignored;
        fs::remove_all(io_path(path_from_utf8(pending.at("stage").get<std::string>())), ignored);
        if (discard) return std::nullopt;
    }
    TrackedDownload item{id, data.at("share"), data.at("share_name"), data.at("remote_path"),
                        path_from_utf8(data.at("destination")), {data.at("directory"), data.at("revision"), {}}};
    for (const auto& entry : data.at("entries")) {
        const auto name = entry.at("path").get<std::string>();
        if (item.baseline.directory) safe_download_path(name);
        else if (!name.empty()) throw std::runtime_error("Invalid tracked file path.");
        item.baseline.entries.emplace(name, DownloadEntry{entry.at("directory"), {entry.at("size"), entry.at("sha256"), {}}});
    }
    return item;
}

TrackedDownload load(const fs::path& registry, const std::string& id) {
    auto item = find(registry, id);
    if (!item) throw std::runtime_error("This download is no longer tracked.");
    return std::move(*item);
}

void commit_update(const fs::path& registry, const TrackedDownload* old, const TrackedDownload& updated,
                   Stage& stage, bool had_target) {
    const auto record = record_path(registry, updated.id);
    auto transaction = encode(old ? *old : updated);
    const json pending{{"target", path_utf8(updated.destination)}, {"stage", path_utf8(stage.path)},
                       {"had_target", had_target}, {"discard_record", !old}};
    transaction["pending"] = pending;
    write_atomically(record, transaction.dump());
    stage.preserve = true;
    try {
        if (had_target) fs::rename(io_path(updated.destination), io_path(stage.path / "old"));
        fs::rename(io_path(stage.path / "new"), io_path(updated.destination));
        write_atomically(record, encode(updated).dump());
        stage.preserve = false;
    } catch (...) {
        try {
            recover(pending);
            if (old) write_atomically(record, encode(*old).dump());
            else fs::remove(record);
            stage.preserve = false;
        } catch (const std::exception& error) {
            throw std::runtime_error("Could not restore the previous download. Recovery files are in "
                                     + path_utf8(stage.path) + ". " + error.what());
        }
        throw;
    }
}

constexpr std::uint64_t gear(unsigned char byte) {
    auto value = std::uint64_t(byte) + 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

using ChunkIndex = std::unordered_map<std::string, FileChunk>;
ChunkIndex index_chunks(std::ifstream& input, const DownloadProgress& progress) {
    constexpr size_t minimum = 256 * 1024, maximum = 4 * 1024 * 1024;
    constexpr auto table = [] {
        std::array<std::uint64_t, 256> values{};
        for (size_t i = 0; i < values.size(); ++i) values[i] = gear(static_cast<unsigned char>(i));
        return values;
    }();
    ChunkIndex index;
    std::uint64_t rolling = 0;
    std::int64_t offset = 0;
    std::string chunk;
    chunk.reserve(maximum);
    const auto save = [&] {
        auto hash = Sha256::of(chunk);
        index.try_emplace(hash, FileChunk{offset, std::int64_t(chunk.size()), hash});
        offset += std::int64_t(chunk.size());
        rolling = 0;
        chunk.clear();
    };
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        require_active(progress);
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            const auto byte = static_cast<unsigned char>(buffer[size_t(i)]);
            chunk.push_back(buffer[size_t(i)]);
            rolling = (rolling << 1) + table[byte];
            if (chunk.size() == maximum || (chunk.size() >= minimum && (rolling & ((1 << 20) - 1)) == 0))
                save();
        }
    }
    if (input.bad()) throw std::runtime_error("Could not read the local file for its update.");
    if (!chunk.empty()) save();
    input.clear();
    return index;
}

bool patch(FilesApi& api, const std::string& share, const std::string& path, const FileMetadata& file,
           const fs::path& base, const fs::path& output, const DownloadProgress& progress) {
    if (file.size < 2 * 1024 * 1024 || fs::file_size(base) < 256 * 1024) return false;
    std::ifstream input(base, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read the local file for its update.");
    const auto have = index_chunks(input, progress);
    require_active(progress);
    const auto expected = api.chunks(share, path, file, [&] { return progress(-1, 0); });
    const auto reused = [&](const auto& chunk) {
        const auto local = have.find(chunk.sha256);
        return local != have.end() && local->second.size == chunk.size;
    };
    std::int64_t missing = 0;
    for (const auto& chunk : expected) if (!reused(chunk)) missing += chunk.size;
    if (missing >= file.size - file.size / 20) return false;

    std::ofstream result(output, std::ios::binary | std::ios::trunc);
    if (!result) throw std::runtime_error("Could not stage the file update.");
    Sha256 actual;
    const auto write = [&](std::string_view bytes) {
        actual.update(bytes.data(), bytes.size());
        result.write(bytes.data(), std::streamsize(bytes.size()));
        if (!result) throw std::runtime_error("Could not write the file update. Check available disk space.");
    };
    size_t i = 0;
    while (i < expected.size()) {
        const auto& chunk = expected[i];
        if (!progress(chunk.offset, file.size)) throw std::runtime_error("Download cancelled.");
        if (reused(chunk)) {
            const auto& local = have.at(chunk.sha256);
            std::string bytes(size_t(local.size), '\0');
            input.seekg(local.offset);
            if (!input.read(bytes.data(), std::streamsize(bytes.size())) || Sha256::of(bytes) != chunk.sha256)
                throw std::runtime_error("The local file changed during the update. Retry.");
            write(bytes);
            ++i;
        } else {
            const auto first = i;
            std::int64_t size = 0;
            while (i < expected.size() && !reused(expected[i]) && size + expected[i].size <= 16 * 1024 * 1024)
                size += expected[i++].size;
            const auto bytes = api.range(share, path, file, chunk.offset, size, [&] {
                return progress(chunk.offset, file.size);
            });
            size_t cursor = 0;
            for (auto j = first; j < i; ++j) {
                const auto view = std::string_view(bytes).substr(cursor, size_t(expected[j].size));
                if (Sha256::of(view) != expected[j].sha256)
                    throw std::runtime_error("The server file changed during the update. Check for updates and retry.");
                write(view);
                cursor += view.size();
            }
        }
    }
    result.close();
    if (!result || actual.hex() != file.sha256) throw std::runtime_error("The updated file failed verification.");
    return true;
}

std::string change_conflict(const std::string& name) {
    return "An update conflicts with local changes at " + (name.empty() ? std::string("the downloaded file") : name) +
           ". Move the local copy aside and retry.";
}

// A second inventory catches running games, editors, and new files changing a
// folder while its replacement is staged.
struct LocalEntry {
    Kind type;
    std::uintmax_t size;
    fs::file_time_type modified;
    bool operator==(const LocalEntry&) const = default;
};
using Inventory = std::map<fs::path, LocalEntry>;

Inventory inventory(const fs::path& root, const fs::path* copy, const DownloadProgress& progress) {
    Inventory result;
    const auto capture = [&](const fs::path& path, const fs::path& relative) {
        require_active(progress);
        const auto type = kind(path);
        if (type != Kind::file && type != Kind::directory)
            throw std::runtime_error("Move the link or special file out of the downloaded folder before updating: " + path_utf8(path));
        const auto size = type == Kind::file ? fs::file_size(path) : 0;
        result.emplace(relative, LocalEntry{type, size, fs::last_write_time(path)});
        if (!copy) return;
        const auto destination = relative.empty() ? *copy : *copy / relative;
        if (type == Kind::directory) fs::create_directory(destination);
        else fs::copy_file(path, destination);
    };
    capture(root, {});
    if (result.begin()->second.type == Kind::directory) {
        for (const auto& item : fs::recursive_directory_iterator(root))
            capture(item.path(), item.path().lexically_relative(root));
    }
    return result;
}

void download_file(FilesApi& api, const fs::path& cache, const std::string& share, const std::string& path,
                   const fs::path& target, const DownloadProgress& progress, const FileMetadata& expected) {
    Desktop::download(api, cache, share, path, target, progress, [&] { return progress(-1, 0); }, &expected);
}

void populate(FilesApi& api, const fs::path& cache, const TrackedDownload& item, const fs::path& target,
              const DownloadProgress& progress) {
    if (item.baseline.directory) fs::create_directory(target);
    std::int64_t total = 0, completed = 0;
    for (const auto& [_, entry] : item.baseline.entries) {
        if (entry.file.size > std::numeric_limits<std::int64_t>::max() - total) throw std::runtime_error("The folder is too large.");
        total += entry.file.size;
    }
    for (const auto& [name, entry] : item.baseline.entries) {
        require_active(progress);
        const auto local = local_member(target, name);
        if (kind(local) != Kind::missing) throw std::runtime_error("Download paths collide on this computer: " + name);
        if (entry.directory) {
            if (!fs::create_directory(local)) throw std::runtime_error("Could not create " + name);
        } else {
            download_file(api, cache, item.share, remote_member(item.remote_path, name), local,
                          [&](std::int64_t received, std::int64_t) {
                              return progress(received < 0 ? received : completed + received, total);
                          }, entry.file);
            completed += entry.file.size;
        }
    }
}

void revise(FilesApi& api, const fs::path& cache, const TrackedDownload& old, const TrackedDownload& updated,
            const fs::path& root, const fs::path& scratch, const DownloadProgress& progress) {
    for (auto it = old.baseline.entries.rbegin(); it != old.baseline.entries.rend(); ++it) {
        require_active(progress);
        const auto& [name, entry] = *it;
        const auto fresh = updated.baseline.entries.find(name);
        if (fresh != updated.baseline.entries.end() && fresh->second.directory == entry.directory) continue;
        const auto local = local_member(root, name);
        const auto type = kind(local);
        if (type == Kind::missing) continue;
        if (entry.directory && type == Kind::directory) {
            if (fs::is_empty(local)) fs::remove(local);
            else if (fresh != updated.baseline.entries.end()) throw std::runtime_error(change_conflict(name));
        } else if (!entry.directory && regular_matches(local, entry.file)) {
            fs::remove(local);
        } else {
            throw std::runtime_error(change_conflict(name));
        }
    }
    for (const auto& [name, entry] : updated.baseline.entries) {
        require_active(progress);
        const auto old_entry = old.baseline.entries.find(name);
        const auto local = local_member(root, name);
        const auto type = kind(local);
        const bool tracked = old_entry != old.baseline.entries.end() && old_entry->second.directory == entry.directory;
        if (entry.directory) {
            if (type == Kind::missing) fs::create_directory(local);
            else if (!tracked || type != Kind::directory) throw std::runtime_error(change_conflict(name));
            continue;
        }
        if (tracked && old_entry->second.file.sha256 == entry.file.sha256 && type != Kind::missing) continue;
        if (type != Kind::missing && (!tracked || !regular_matches(local, old_entry->second.file)))
            throw std::runtime_error(change_conflict(name));

        const auto remote = remote_member(updated.remote_path, name);
        if (type != Kind::file || !patch(api, updated.share, remote, entry.file, local, scratch, progress))
            download_file(api, cache, updated.share, remote, scratch, progress, entry.file);
        if (type == Kind::file) fs::remove(local);
        fs::rename(scratch, local);
    }
}

bool component_equal(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return left == right;
#endif
}

bool covers(const fs::path& parent, const fs::path& target) {
    const auto common = std::mismatch(parent.begin(), parent.end(), target.begin(), target.end(), component_equal);
    return common.first == parent.end();
}

std::optional<TrackedDownload> check_destination(const fs::path& registry, const fs::path& destination,
                                                 const std::string& self, bool replace_same = false) {
    std::optional<TrackedDownload> previous;
    for (const auto& entry : fs::directory_iterator(registry)) {
        if (entry.path().extension() != ".json") continue;
        const auto id = path_utf8(entry.path().stem());
        if (id == self) continue;
        auto item = find(registry, id);
        if (!item) continue;
        const auto contains = covers(item->destination, destination);
        const auto contained = covers(destination, item->destination);
        if (replace_same && contains && contained && !previous) {
            previous = std::move(item);
        } else if (contains || contained) {
            throw std::runtime_error("Choose a location outside your other tracked downloads.");
        }
    }
    return previous;
}

} // namespace

FileDownloads::FileDownloads(Store& store)
    : api_(store.files_api()), cache_(store.directory() / "downloads") {
    const auto account = store.account();
    if (account.session.token.empty()) throw std::runtime_error("Sign in to browse and update downloads.");
    registry_ = store.directory() / "tracked-downloads" / Sha256::of(account.server + '\0' + account.session.username);
    fs::create_directories(registry_);
}

void FileDownloads::download(const Share& share, const std::string& path, bool directory,
                             const fs::path& destination, const DownloadProgress& progress) {
    const ProfileLock lock(registry_ / "items.lock");
    const auto target = destination_path(destination);
    const auto before = kind(target);
    if (before != Kind::missing && (directory || before != Kind::file))
        throw std::runtime_error("Choose an unused destination for this download.");
    const auto current_shares = api_.shares();
    const auto current_share = std::ranges::find(current_shares, share.id, &Share::id);
    if (current_share == current_shares.end()) throw std::runtime_error("This file server is unavailable.");
    const auto track = current_share->supports_updates;
    if (!directory && !track) {
        Desktop::download(api_, cache_, share.id, path, target, progress, [&] { return progress(-1, 0); });
        return;
    }
    const auto before_edit = before == Kind::file ? fs::last_write_time(io_path(target)) : fs::file_time_type{};
    const auto active = [&] { return progress(-1, 0); };
    auto manifest = api_.manifest(share.id, path, track, active);
    if (manifest.directory != directory) throw std::runtime_error("The shared item changed type. Refresh and try again.");
    const auto previous = track ? check_destination(registry_, target, {}, true) : std::nullopt;
    const auto id = previous ? previous->id : random_id();
    TrackedDownload item{id, share.id, current_share->name, path, target, std::move(manifest)};
    Stage stage(target);
    const auto staged = io_path(stage.path / "new");
    populate(api_, cache_, item, staged, progress);
    if (directory && api_.manifest(share.id, path, track, active).revision != item.baseline.revision)
        throw std::runtime_error("The shared folder changed during the download. Try again.");
    require_active(progress);
    if (kind(target) != before || (before == Kind::file && fs::last_write_time(io_path(target)) != before_edit))
        throw std::runtime_error("The destination changed during the download.");
    if (track) {
        commit_update(registry_, previous ? &*previous : nullptr, item, stage, before == Kind::file);
    } else {
        fs::rename(staged, io_path(target));
    }
}

std::vector<DownloadCheck> FileDownloads::check() {
    const ProfileLock lock(registry_ / "items.lock");
    std::map<std::string, Share> shares;
    std::string share_error;
    try {
        for (auto share : api_.shares()) shares.emplace(share.id, std::move(share));
    } catch (const std::exception& error) {
        share_error = error.what();
    }
    std::vector<DownloadCheck> checks;
    for (const auto& entry : fs::directory_iterator(registry_)) {
        if (entry.path().extension() != ".json") continue;
        const auto id = path_utf8(entry.path().stem());
        auto item = find(registry_, id);
        if (!item) continue;
        DownloadCheck check{std::move(*item), DownloadStatus::current, false, {}};
        const auto share = shares.find(check.item.share);
        const auto local = kind(check.item.destination);
        const bool missing = local != (check.item.baseline.directory ? Kind::directory : Kind::file);
        if (share != shares.end()) check.item.share_name = share->second.name;
        try {
            if (!share_error.empty()) {
                check.status = DownloadStatus::check_failed;
                check.error = share_error;
            } else if (share == shares.end()) check.status = DownloadStatus::unavailable;
            else if (!share->second.supports_updates) check.status = DownloadStatus::disabled;
            else {
                const auto manifest = api_.manifest(check.item.share, check.item.remote_path, true);
                if (manifest.directory != check.item.baseline.directory) {
                    check.status = DownloadStatus::unavailable;
                    check.error = "The shared item changed type.";
                } else {
                    check.can_download = true;
                    if (manifest.revision != check.item.baseline.revision) check.status = DownloadStatus::available;
                }
            }
        } catch (const ApiError& error) {
            check.status = error.code == "updates_disabled" ? DownloadStatus::disabled :
                error.status == 404 || error.status == 403 ? DownloadStatus::unavailable : DownloadStatus::check_failed;
            check.error = error.what();
        } catch (const std::exception& error) {
            check.status = DownloadStatus::check_failed;
            check.error = error.what();
        }
        if (missing) check.status = DownloadStatus::missing;
        checks.push_back(std::move(check));
    }
    return checks;
}

void FileDownloads::replace(const TrackedDownload& item, const fs::path& destination, bool repair,
                            const DownloadProgress& progress) {
    const auto shares = api_.shares();
    if (std::ranges::none_of(shares, [&](const auto& share) { return share.id == item.share && share.supports_updates; }))
        throw std::runtime_error("Updates are unavailable for this file server.");
    const auto target = destination_path(destination);
    if (repair && kind(target) != Kind::missing) throw std::runtime_error("Choose an unused destination.");
    if (!repair && kind(target) != (item.baseline.directory ? Kind::directory : Kind::file))
        throw std::runtime_error("The download moved or was deleted. Locate it or download it again.");
    check_destination(registry_, target, item.id);

    auto updated = item;
    updated.destination = target;
    const auto active = [&] { return progress(-1, 0); };
    updated.baseline = api_.manifest(item.share, item.remote_path, true, active);
    if (updated.baseline.directory != item.baseline.directory) throw std::runtime_error("The shared item changed type.");
    if (!repair && updated.baseline.revision == item.baseline.revision) return;

    Stage stage(target);
    const auto staged = io_path(stage.path / "new");
    Inventory before;
    if (repair) {
        populate(api_, cache_, updated, staged, progress);
    } else {
        before = inventory(io_path(target), &staged, progress);
        revise(api_, cache_, item, updated, staged, io_path(stage.path / "file"), progress);
    }

    if (api_.manifest(item.share, item.remote_path, true, active).revision != updated.baseline.revision)
        throw std::runtime_error("The shared item changed during the update. Check for updates and retry.");
    if ((repair && kind(target) != Kind::missing) || (!repair && inventory(io_path(target), nullptr, progress) != before))
        throw std::runtime_error("The local download changed during the update. Close it and retry.");
    require_active(progress);
    commit_update(registry_, &item, updated, stage, !repair);
}

void FileDownloads::update(const std::string& id, const DownloadProgress& progress) {
    const ProfileLock lock(registry_ / "items.lock");
    const auto item = load(registry_, id);
    replace(item, item.destination, false, progress);
}

void FileDownloads::redownload(const std::string& id, const fs::path& destination, const DownloadProgress& progress) {
    const ProfileLock lock(registry_ / "items.lock");
    const auto item = load(registry_, id);
    replace(item, destination, true, progress);
}

void FileDownloads::locate(const std::string& id, const fs::path& destination) {
    const ProfileLock lock(registry_ / "items.lock");
    auto item = load(registry_, id);
    const auto target = destination_path(destination);
    if (kind(target) != (item.baseline.directory ? Kind::directory : Kind::file))
        throw std::runtime_error("Choose an existing copy of the downloaded item.");
    check_destination(registry_, target, item.id);
    // Retain the previous baseline so edits in the relocated copy still get
    // the same conflict protection on its next update.
    item.destination = target;
    write_atomically(record_path(registry_, id), encode(item).dump());
}

void FileDownloads::forget(const std::string& id) {
    const ProfileLock lock(registry_ / "items.lock");
    load(registry_, id);
    fs::remove(record_path(registry_, id));
}

} // namespace loki
