#include "store.hpp"
#include "files_api.hpp"

#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace loki {

void write_atomically(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out.flush()) throw std::runtime_error("Cannot write " + file.string());
    }
    fs::rename(temporary, file);
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

namespace {
std::string utf8(const fs::path& path) {
    auto text = path.u8string();
    return {text.begin(), text.end()};
}
fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
}  // namespace

Store::Store(fs::path directory) : directory_(std::move(directory)) {
    fs::create_directories(directory_ / "index");
    const auto file = directory_ / "config.json";
    auto o = json::parse(read_text(file), nullptr, false);
    if (!o.is_object()) o = json::object();
    // Earlier clients stored a second file-access login. Reuse it only when no
    // primary login exists; a signed-in sync account always takes precedence.
    if (o.value("token", "").empty()) {
        const auto legacy = json::parse(read_text(directory_ / "files.json"), nullptr, false);
        if (legacy.is_object() && !legacy.value("token", "").empty()) {
            for (const auto* key : {"server", "token", "username", "display_name"}) o[key] = legacy.value(key, "");
            o["can_sync"] = legacy.value("can_sync", true);
            write_atomically(file, o.dump(2));
            write_atomically(directory_ / "files.json", "{}");
        }
    }
    server_ = o.value("server", "");
    token_ = o.value("token", "");
    username_ = o.value("username", "");
    display_name_ = o.value("display_name", "");
    can_sync_ = o.value("can_sync", true);
    if (o.contains("mappings")) {
        for (const auto& [id, m] : o["mappings"].items()) {
            mappings_[id] = {id, from_utf8(m.value("path", "")), m.value("last_sync", std::int64_t(0)),
                             m.value("last_result", ""), m.value("failed", false)};
        }
    }
}

void Store::save() const {
    json mappings = json::object();
    for (const auto& [id, m] : mappings_) {
        mappings[id] = {{"path", utf8(m.local)},
                        {"last_sync", m.last_sync},
                        {"last_result", m.last_result},
                        {"failed", m.failed}};
    }
    json o{{"server", server_},
           {"token", token_},
           {"username", username_},
           {"display_name", display_name_},
           {"can_sync", can_sync_},
           {"mappings", mappings}};
    write_atomically(directory_ / "config.json", o.dump(2));
}

std::string Store::server() const {
    std::lock_guard lock(mutex_);
    return server_;
}
std::string Store::token() const {
    std::lock_guard lock(mutex_);
    return token_;
}
std::string Store::display_name() const {
    std::lock_guard lock(mutex_);
    return display_name_.empty() ? username_ : display_name_;
}
bool Store::signed_in() const {
    std::lock_guard lock(mutex_);
    return !server_.empty() && !token_.empty();
}
bool Store::can_sync() const { std::lock_guard lock(mutex_); return can_sync_; }
Session Store::session() const {
    std::lock_guard lock(mutex_);
    return {token_, "", username_, display_name_, can_sync_};
}
FilesApi Store::files_api() const { std::lock_guard lock(mutex_); return FilesApi(server_,token_); }
void Store::set_sync_access(bool allowed) { std::lock_guard lock(mutex_); can_sync_=allowed; save(); }
Api Store::api() const {
    std::lock_guard lock(mutex_);
    return Api(server_, token_);
}

void Store::sign_in(const std::string& server, const Session& session) {
    std::lock_guard lock(mutex_);
    server_ = Api::normalize(server);
    token_ = session.token;
    username_ = session.username;
    display_name_ = session.display_name;
    can_sync_ = session.can_sync;
    save();
}

void Store::sign_out() {
    std::lock_guard lock(mutex_);
    server_.clear();
    token_.clear();
    username_.clear();
    display_name_.clear();
    write_atomically(directory_ / "files.json", "{}");
    mappings_.clear();
    std::error_code ignored;
    fs::remove_all(directory_ / "index", ignored);
    fs::create_directories(directory_ / "index", ignored);
    save();
}

std::map<std::string, Mapping> Store::mappings() const {
    std::lock_guard lock(mutex_);
    return mappings_;
}

std::optional<Mapping> Store::mapping(const std::string& folder_id) const {
    std::lock_guard lock(mutex_);
    auto it = mappings_.find(folder_id);
    if (it == mappings_.end()) return std::nullopt;
    return it->second;
}

void Store::save_mapping(const Mapping& mapping) {
    std::lock_guard lock(mutex_);
    mappings_[mapping.folder_id] = mapping;
    save();
}

bool Store::while_current(const Mapping& mapping, const std::function<void()>& block) {
    std::lock_guard lock(mutex_);
    const auto it = mappings_.find(mapping.folder_id);
    if (it == mappings_.end() || it->second.local != mapping.local) return false;
    block();
    return true;
}

bool Store::record_result(const Mapping& mapping) {
    std::lock_guard lock(mutex_);
    const auto it = mappings_.find(mapping.folder_id);
    if (it == mappings_.end() || it->second.local != mapping.local) return false;
    it->second = mapping;
    save();
    return true;
}

void Store::remove_mapping(const std::string& folder_id) {
    std::lock_guard lock(mutex_);
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
