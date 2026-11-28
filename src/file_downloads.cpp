#include "file_downloads.hpp"

#include "desktop.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

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

void require_active(const DownloadProgress& progress) {
    if (!progress(0, 0)) throw std::runtime_error("Download cancelled.");
}

/** Downloads every manifest entry into the new folder `target`. */
void populate(FilesApi& api, const fs::path& cache, const std::string& share, const std::string& remote,
              const DownloadManifest& manifest, const fs::path& target, const DownloadProgress& progress) {
    fs::create_directory(target);
    std::int64_t total = 0, completed = 0;
    for (const auto& [_, entry] : manifest.entries) {
        if (entry.file.size > std::numeric_limits<std::int64_t>::max() - total) throw std::runtime_error("The folder is too large.");
        total += entry.file.size;
    }
    for (const auto& [name, entry] : manifest.entries) {
        require_active(progress);
        const auto local = local_member(target, name);
        if (kind(local) != Kind::missing) throw std::runtime_error("Download paths collide on this computer: " + name);
        if (entry.directory) {
            if (!fs::create_directory(local)) throw std::runtime_error("Could not create " + name);
            continue;
        }
        Desktop::download(api, cache, share, remote_member(remote, name), local,
                          [&](std::int64_t received, std::int64_t) {
                              return progress(received < 0 ? received : completed + received, total);
                          },
                          [&] { return progress(-1, 0); }, &entry.file);
        completed += entry.file.size;
    }
}

} // namespace

FileDownloads::FileDownloads(Store& store)
    : api_(store.files_api()), cache_(store.directory() / "downloads") {
    const auto account = store.account();
    if (account.session.token.empty()) throw std::runtime_error("Sign in to browse and download files.");
    // Download tracking for updates was removed in 0.6.0; drop its old records.
    std::error_code ignored;
    fs::remove_all(store.directory() / "tracked-downloads", ignored);
}

void FileDownloads::download(const Share& share, const std::string& path, bool directory,
                             const fs::path& destination, const DownloadProgress& progress) {
    const auto target = destination_path(destination);
    const auto before = kind(target);
    if (!directory) {
        if (before != Kind::missing && before != Kind::file) throw std::runtime_error("Choose an unused destination for this download.");
        Desktop::download(api_, cache_, share.id, path, target, progress, [&] { return progress(-1, 0); });
        return;
    }
    if (before != Kind::missing) throw std::runtime_error("Choose an unused destination for this download.");
    const auto active = [&] { return progress(-1, 0); };
    const auto manifest = api_.manifest(share.id, path, active);
    if (!manifest.directory) throw std::runtime_error("The shared item changed type. Refresh and try again.");
    Stage stage(target);
    const auto staged = io_path(stage.path / "new");
    populate(api_, cache_, share.id, path, manifest, staged, progress);
    if (api_.manifest(share.id, path, active).revision != manifest.revision)
        throw std::runtime_error("The shared folder changed during the download. Try again.");
    require_active(progress);
    if (kind(target) != Kind::missing) throw std::runtime_error("The destination changed during the download.");
    fs::rename(staged, io_path(target));
}

} // namespace loki
