#include "store.hpp"
#include "files_api.hpp"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace loki {

ProfileLock::ProfileLock(const fs::path& path) {
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open the LOKI profile lock.");
    OVERLAPPED range{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range)) {
        CloseHandle(file);
        throw std::runtime_error("Cannot lock the LOKI profile.");
    }
    handle_ = file;
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ < 0) throw std::runtime_error("Cannot open the LOKI profile lock.");
    int result;
    do { result = flock(fd_, LOCK_EX); } while (result != 0 && errno == EINTR);
    if (result != 0) {
        close(fd_);
        throw std::runtime_error("Cannot lock the LOKI profile.");
    }
#endif
}

ProfileLock::~ProfileLock() {
#ifdef _WIN32
    CloseHandle(handle_);
#else
    close(fd_);
#endif
}

void write_atomically(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
#ifdef _WIN32
    wchar_t name[MAX_PATH];
    if (!GetTempFileNameW(file.parent_path().c_str(), L"lki", 0, name))
        throw std::runtime_error("Cannot create a temporary LOKI file.");
    const fs::path temporary(name);
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out || !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot write " + file.string());
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
#else
    auto temporary = file.string() + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error("Cannot create a temporary LOKI file.");
    FILE* out = fdopen(fd, "wb");
    const bool written = out && std::fwrite(text.data(), 1, text.size(), out) == text.size();
    const auto closed = out ? std::fclose(out) : close(fd);
    if (!written || closed != 0 || std::rename(temporary.c_str(), file.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("Cannot write " + file.string());
    }
#endif
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string path_utf8(const fs::path& path) {
    auto text = path.u8string();
    return {text.begin(), text.end()};
}
fs::path path_from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

fs::path user_directory() {
#ifdef _WIN32
    if (const auto* dir = _wgetenv(L"LOKI_DESKTOP_CONFIG_DIR"); dir && *dir) return fs::path(dir);
    if (const auto* dir = _wgetenv(L"APPDATA"); dir && *dir) return fs::path(dir) / "LOKI Sync";
#else
    if (const auto* dir = std::getenv("LOKI_DESKTOP_CONFIG_DIR"); dir && *dir) return fs::path(dir);
    if (const auto* home = std::getenv("HOME"); home && *home) {
        const auto root = fs::path(home);
#ifdef __APPLE__
        return root / "Library" / "Application Support" / "LOKI Sync";
#else
        const auto old = root / ".LOKI Sync";
        if (fs::exists(old / "config.json") || fs::exists(old / "files.json")) return old;
        const auto* data = std::getenv("XDG_DATA_HOME");
        return (data && *data ? fs::path(data) : root / ".local" / "share") / "LOKI Sync";
#endif
    }
#endif
    throw std::runtime_error("Set LOKI_DESKTOP_CONFIG_DIR to the desktop profile directory.");
}

namespace {
#ifdef _WIN32
std::string protect(std::string value, bool encrypt) {
    constexpr std::string_view digits = "0123456789abcdef";
    if (!encrypt) {
        const auto encoded = std::move(value);
        if (encoded.size() % 2) throw std::runtime_error("Invalid LOKI credential.");
        value.clear();
        for (size_t i = 0; i < encoded.size(); i += 2) {
            const auto high = digits.find(encoded[i]), low = digits.find(encoded[i + 1]);
            if (high == digits.npos || low == digits.npos) throw std::runtime_error("Invalid LOKI credential.");
            value.push_back(char(high * 16 + low));
        }
    }
    DATA_BLOB input{DWORD(value.size()), reinterpret_cast<BYTE*>(value.data())}, result{};
    const auto ok = encrypt
        ? CryptProtectData(&input, L"LOKI desktop session", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result)
        : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (!ok) throw std::runtime_error("Cannot access the Windows-protected LOKI credential.");
    if (encrypt) {
        value.clear();
        for (DWORD i = 0; i < result.cbData; ++i) {
            value.push_back(digits[result.pbData[i] >> 4]);
            value.push_back(digits[result.pbData[i] & 15]);
        }
    } else {
        value.assign(reinterpret_cast<char*>(result.pbData), result.cbData);
        SecureZeroMemory(result.pbData, result.cbData);
    }
    LocalFree(result.pbData);
    return value;
}
#endif

std::string load_token(const json& values) {
    auto token = values.value("token", "");
#ifdef _WIN32
    if (!token.empty() && values.value("protected", false)) token = protect(std::move(token), false);
#endif
    return token;
}
}  // namespace

Store::Store(fs::path directory) : directory_(std::move(directory)) {
    fs::create_directories(directory_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    fs::create_directories(directory_ / "index");
    if (!token_.empty()) return;

    const auto legacy_file = directory_ / "files.json";
    const auto credentials = json::parse(read_text(legacy_file), nullptr, false);
    if (credentials.is_object() && !credentials.value("token", "").empty()) {
        server_ = credentials.value("server", "");
        token_ = load_token(credentials);
        username_ = credentials.value("username", "");
        display_name_ = credentials.value("display_name", "");
        can_sync_ = credentials.value("can_sync", true);
        save();
        write_atomically(legacy_file, "{}");
    } else if (!fs::exists(directory_ / "config.json")) {
        save();
    }
}

void Store::load() const {
    auto o = json::parse(read_text(directory_ / "config.json"), nullptr, false);
    if (!o.is_object()) {
        if (fs::exists(directory_ / "config.json")) throw std::runtime_error("Cannot read the LOKI profile.");
        o = json::object();
    } else if (o.value("profile", "desktop") != "desktop") {
        throw std::runtime_error("Choose a LOKI Desktop profile directory.");
    }

    server_ = o.value("server", "");
    token_ = load_token(o);
    username_ = o.value("username", "");
    display_name_ = o.value("display_name", "");
    device_id_ = o.value("device_id", "");
    scope_ = o.value("scope", "");
    can_sync_ = o.value("can_sync", true);
    mappings_.clear();
    if (o.contains("mappings")) {
        for (const auto& [id, m] : o["mappings"].items()) {
            mappings_[id] = {id, path_from_utf8(m.value("path", "")), m.value("last_sync", std::int64_t(0)),
                             m.value("last_result", ""), m.value("failed", false)};
        }
    }
}

void Store::save() const {
    json mappings = json::object();
    for (const auto& [id, m] : mappings_) {
        mappings[id] = {{"path", path_utf8(m.local)},
                        {"last_sync", m.last_sync},
                        {"last_result", m.last_result},
                        {"failed", m.failed}};
    }
    json o{{"profile", "desktop"},
           {"server", server_},
           {"token", token_},
           {"username", username_},
           {"display_name", display_name_},
           {"scope", scope_},
           {"device_id", device_id_},
           {"can_sync", can_sync_},
           {"mappings", mappings}};
#ifdef _WIN32
    if (!token_.empty()) {
        o["token"] = protect(token_, true);
        o["protected"] = true;
    }
#endif
    write_atomically(directory_ / "config.json", o.dump(2));
}

std::string Store::server() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return server_;
}
std::string Store::token() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return token_;
}
std::string Store::display_name() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return display_name_.empty() ? username_ : display_name_;
}
bool Store::signed_in() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return !server_.empty() && !token_.empty();
}
bool Store::can_sync() const { return session().can_sync; }
Session Store::session() const { return account().session; }
StoredAccount Store::account() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return {server_, {token_, device_id_, username_, display_name_, can_sync_, scope_}};
}
FilesApi Store::files_api() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return FilesApi(server_, token_);
}
void Store::set_sync_access(bool allowed) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    if (!token_.empty() && can_sync_ != allowed) { can_sync_ = allowed; save(); }
}
Api Store::api() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return Api(server_, token_);
}

void Store::sign_in(const std::string& server, const Session& session, const std::optional<std::string>& expected) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    if (expected && token_ != *expected) throw std::runtime_error("The shared sign-in changed. Try again.");
    const auto normalized = Api::normalize(server);
    if (!token_.empty() && (server_ != normalized || (!username_.empty() && username_ != session.username))) {
        throw std::runtime_error("Sign out before switching accounts or servers.");
    }
    server_ = normalized;
    token_ = session.token;
    device_id_ = session.device_id;
    scope_ = session.scope;
    username_ = session.username;
    display_name_ = session.display_name;
    can_sync_ = session.can_sync;
    save();
}

void Store::sign_out(const std::optional<std::string>& expected) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    if (expected && token_ != *expected) throw std::runtime_error("The shared sign-in changed. Try again.");
    server_.clear();
    token_.clear();
    username_.clear();
    display_name_.clear();
    device_id_.clear();
    scope_.clear();
    write_atomically(directory_ / "files.json", "{}");
    mappings_.clear();
    std::error_code ignored;
    fs::remove_all(directory_ / "index", ignored);
    fs::create_directories(directory_ / "index", ignored);
    save();
}

std::map<std::string, Mapping> Store::mappings() const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    return mappings_;
}

std::optional<Mapping> Store::mapping(const std::string& folder_id) const {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    auto it = mappings_.find(folder_id);
    if (it == mappings_.end()) return std::nullopt;
    return it->second;
}

void Store::save_mapping(const Mapping& mapping) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    const auto old = mappings_.find(mapping.folder_id);
    if (old != mappings_.end() && old->second.local == mapping.local && mapping.last_result.empty()) return;
    if (old == mappings_.end() || old->second.local != mapping.local) {
        std::error_code ignored;
        fs::remove(index_file(mapping.folder_id), ignored);
    }
    mappings_[mapping.folder_id] = mapping;
    save();
}

bool Store::while_current(const Mapping& mapping, const std::function<void()>& block) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    const auto it = mappings_.find(mapping.folder_id);
    if (it == mappings_.end() || it->second.local != mapping.local) return false;
    block();
    return true;
}

bool Store::record_result(const Mapping& mapping) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    const auto it = mappings_.find(mapping.folder_id);
    if (it == mappings_.end() || it->second.local != mapping.local) return false;
    it->second = mapping;
    save();
    return true;
}

void Store::remove_mapping(const std::string& folder_id) {
    std::lock_guard lock(mutex_);
    ProfileLock transaction(directory_ / "profile.lock");
    load();
    mappings_.erase(folder_id);
    std::error_code ignored;
    fs::remove(index_file(folder_id), ignored);
    save();
}

fs::path Store::index_file(const std::string& folder_id) const {
    std::string safe;
    for (char c : folder_id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') safe += c;
    }
    return directory_ / "index" / (safe + ".json");
}

}  // namespace loki
