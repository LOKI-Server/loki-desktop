#include "api.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace loki {
namespace {

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string escape(const std::string& value) {
    std::unique_ptr<char, decltype(&curl_free)> out(curl_easy_escape(nullptr, value.c_str(), int(value.size())),
                                                    curl_free);
    return out ? std::string(out.get()) : value;
}

std::FILE* open_file(const std::filesystem::path& path, bool write) {
#ifdef _WIN32
    return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

size_t read_file(char* data, size_t size, size_t count, void* source) {
    return std::fread(data, size, count, static_cast<std::FILE*>(source)) * size;
}

/** Successful downloads stream to the file; error bodies are always kept as text. */
struct Sink {
    std::string* text;
    std::FILE* file;
    CURL* curl;
};
size_t write_sink(char* data, size_t size, size_t count, void* target) {
    auto* sink = static_cast<Sink*>(target);
    long status = 0;
    curl_easy_getinfo(sink->curl, CURLINFO_RESPONSE_CODE, &status);
    if (sink->file && status >= 200 && status < 300) return std::fwrite(data, size, count, sink->file) * size;
    sink->text->append(data, size * count);
    return size * count;
}

RemoteChange change(const json& o, const std::string& path = {}) {
    RemoteChange c;
    c.path = path.empty() ? o.at("path").get<std::string>() : path;
    c.version = o.at("version").get<std::int64_t>();
    if (o.contains("hash") && !o.at("hash").is_null()) c.hash = o.at("hash").get<std::string>();
    c.size = o.value("size", std::int64_t(0));
    c.mtime = o.value("mtime", std::int64_t(0));
    c.seq = o.value("seq", std::int64_t(0));
    return c;
}

Session session(const json& o) {
    const auto& user = o.at("user");
    const std::string username = user.at("username");
    return {o.at("token"), o.at("device").at("id"), username, user.value("display_name", username), o.value("access", json::object()).value("sync", true)};
}

Folder folder(const json& o) {
    return {o.at("id"), o.at("name"), o.value("kind", "generic"), o.value("keep_versions", 10),
            o.value("seq", std::int64_t(0))};
}

}  // namespace

Api::Api(std::string base_url, std::string token) : base_(normalize(std::move(base_url))), token_(std::move(token)) {
    global_init();
}

std::string Api::normalize(std::string url) {
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    while (!url.empty() && url.front() == ' ') url.erase(url.begin());
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) url = "https://" + url;
    return url;
}

Api::Response Api::request(const std::string& method, const std::string& path, const std::string* body,
                           const std::filesystem::path* upload, const std::filesystem::path* download) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Could not start a network request.");
    auto* h = curl.get();
    const auto url = base_ + path;
    Response response;

    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json");
    if (!token_.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token_).c_str());

    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    // Give up on stalled transfers without capping long uploads.
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 90L);
    if (method == "HEAD") curl_easy_setopt(h, CURLOPT_NOBODY, 1L);

    std::unique_ptr<std::FILE, decltype(&std::fclose)> in(nullptr, std::fclose);
    std::unique_ptr<std::FILE, decltype(&std::fclose)> out(nullptr, std::fclose);
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body->size()));
    } else if (upload) {
        in.reset(open_file(*upload, false));
        if (!in) throw std::runtime_error("Cannot read " + upload->string());
        headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h, CURLOPT_READFUNCTION, read_file);
        curl_easy_setopt(h, CURLOPT_READDATA, in.get());
        curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, curl_off_t(std::filesystem::file_size(*upload)));
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(headers, curl_slist_free_all);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    if (download) {
        out.reset(open_file(*download, true));
        if (!out) throw std::runtime_error("Cannot write " + download->string());
    }
    Sink sink{&response.body, out.get(), h};
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_sink);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);

    const auto code = curl_easy_perform(h);
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("Could not reach the server: ") + curl_easy_strerror(code));
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

void Api::fail(const Response& r) {
    auto o = json::parse(r.body, nullptr, false);
    std::string message = "The server answered " + std::to_string(r.status) + ".";
    std::string code;
    if (o.is_object()) {
        if (o.contains("error") && o["error"].is_string()) message = o["error"];
        if (o.contains("code") && o["code"].is_string()) code = o["code"];
    }
    throw ApiError(r.status, code, message);
}

std::string Api::ok(const std::string& method, const std::string& path, const std::string* body) {
    auto r = request(method, path, body);
    if (r.status < 200 || r.status >= 300) fail(r);
    return r.body;
}

Discovery Api::discover() {
    auto o = json::parse(ok("GET", "/auth/v1"));
    return {o.value("name", "LOKI"), o.value("protocol", 0), o.value("max_file_bytes", std::int64_t(0))};
}

Pairing Api::pair(const std::string& device_name, const std::string& platform) {
    const auto body = json{{"name", device_name}, {"platform", platform}}.dump();
    auto o = json::parse(ok("POST", "/auth/v1/pair", &body));
    return {o.at("device_code"),       o.at("user_code"),       o.at("verification_uri"),
            o.at("verification_uri_complete"), o.value("interval", 5), o.value("expires_in", 600)};
}

std::optional<Session> Api::poll(const std::string& device_code) {
    const auto body = json{{"device_code", device_code}}.dump();
    try {
        return session(json::parse(ok("POST", "/auth/v1/pair/poll", &body)));
    } catch (const ApiError& e) {
        if (e.code == "authorization_pending" || e.code == "slow_down") return std::nullopt;
        throw;
    }
}

Session Api::login(const std::string& username, const std::string& password, const std::string& code,
                   const std::string& device_name, const std::string& platform) {
    const auto body = json{{"username", username}, {"password", password}, {"code", code},
                           {"name", device_name},  {"platform", platform}}
                          .dump();
    return session(json::parse(ok("POST", "/auth/v1/login", &body)));
}

bool Api::sync_access() { return json::parse(ok("GET", "/auth/v1/me")).at("access").at("sync").get<bool>(); }

Quota Api::quota() {
    auto o = json::parse(ok("GET", "/sync/v1/me"));
    return {o.at("quota").at("used"), o.at("quota").at("limit"), o.value("max_file_bytes", std::int64_t(0))};
}

void Api::sign_out() { ok("DELETE", "/auth/v1/device"); }

std::vector<Folder> Api::folders() {
    std::vector<Folder> out;
    for (const auto& o : json::parse(ok("GET", "/sync/v1/folders"))) out.push_back(folder(o));
    return out;
}

Folder Api::create_folder(const std::string& name, const std::string& kind) {
    const auto body = json{{"name", name}, {"kind", kind}}.dump();
    return folder(json::parse(ok("POST", "/sync/v1/folders", &body)));
}

Feed Api::changes(const std::string& folder, std::int64_t since, int limit) {
    auto o = json::parse(ok("GET", "/sync/v1/folders/" + escape(folder) + "/changes?since=" + std::to_string(since) +
                                       "&limit=" + std::to_string(limit)));
    Feed feed{o.at("seq"), o.value("more", false), o.value("reset", false), {}};
    for (const auto& c : o.at("changes")) feed.changes.push_back(change(c));
    return feed;
}

Watch Api::watch(const std::string& state, int wait) {
    auto o = json::parse(ok("GET", "/sync/v1/watch?wait=" + std::to_string(wait) + "&state=" + escape(state)));
    Watch result{o.at("state"), o.value("changed", false), {}};
    for (const auto& f : o.at("folders")) result.folders.emplace_back(f.at("id"), f.at("seq"));
    return result;
}

bool Api::has_blob(const std::string& hash) {
    auto r = request("HEAD", "/sync/v1/blobs/" + hash);
    if (r.status == 200) return true;
    if (r.status == 404) return false;
    fail(r);
}

void Api::upload_blob(const std::string& hash, const std::filesystem::path& file) {
    auto r = request("PUT", "/sync/v1/blobs/" + hash, nullptr, &file);
    if (r.status < 200 || r.status >= 300) fail(r);
}

void Api::download_blob(const std::string& hash, const std::filesystem::path& file) {
    auto r = request("GET", "/sync/v1/blobs/" + hash, nullptr, nullptr, &file);
    if (r.status != 200) fail(r);
}

std::vector<CommitResult> Api::commit(const std::string& folder, const std::vector<LocalChange>& changes,
                                      const std::string& on_conflict) {
    auto list = json::array();
    for (const auto& c : changes) {
        json item{{"path", c.path}, {"base_version", c.base_version}, {"mtime", c.mtime}};
        if (c.hash) {
            item["hash"] = *c.hash;
            item["size"] = c.size;
        } else {
            item["hash"] = nullptr;
        }
        list.push_back(std::move(item));
    }
    const auto body = json{{"changes", list}, {"on_conflict", on_conflict}}.dump();
    auto o = json::parse(ok("POST", "/sync/v1/folders/" + escape(folder) + "/commit", &body));
    std::vector<CommitResult> results;
    for (const auto& r : o.at("results")) {
        CommitResult result;
        result.path = r.at("path");
        const std::string status = r.at("status");
        if (r.contains("current") && r["current"].is_object()) result.current = change(r["current"], result.path);
        if (status == "ok") {
            result.status = CommitResult::Status::Ok;
            result.version = r.at("version");
        } else if (status == "stashed") {
            result.status = CommitResult::Status::Stashed;
        } else if (status == "rejected") {
            result.status = CommitResult::Status::Rejected;
            result.code = r.value("code", "");
            result.error = r.value("error", "");
        }
        results.push_back(std::move(result));
    }
    return results;
}

}  // namespace loki
