#include "files_api.hpp"
#include "sha256.hpp"
#include "store.hpp"
#include <curl/curl.h>
#include <cctype>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;
namespace loki {
namespace {
std::string enc(const std::string& text) {
    std::unique_ptr<char, decltype(&curl_free)> value(curl_easy_escape(nullptr, text.c_str(), int(text.size())), curl_free);
    if (!value) throw std::runtime_error("Could not encode the file path.");
    return value.get();
}
std::string route(const std::string& share, const std::string& endpoint, const std::string& path) {
    return "/files/v1/shares/" + enc(share) + "/" + endpoint + "?path=" + enc(path);
}
struct Transfer {
    CURL* curl;
    fs::path path;
    std::ofstream output;
    std::string error_body, content_range, error;
    std::int64_t offset, expected, received = 0;
    bool started = false;
    const std::function<bool(std::int64_t, std::int64_t)>* progress;
};
size_t headers(char* data, size_t size, size_t count, void* target) {
    auto& t = *static_cast<Transfer*>(target);
    try {
        std::string line(data, size * count);
        if (line.rfind("HTTP/", 0) == 0) t.content_range.clear();
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (key == "content-range") {
                auto value = line.substr(colon + 1);
                const auto first = value.find_first_not_of(" \t");
                const auto last = value.find_last_not_of(" \t\r\n");
                t.content_range = first == std::string::npos ? "" : value.substr(first, last-first+1);
            }
        }
        return size * count;
    } catch (...) { return 0; }
}
size_t body(char* data, size_t size, size_t count, void* target) {
    auto& t = *static_cast<Transfer*>(target);
    const auto bytes = size * count;
    try {
        long status = 0;
        curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &status);
        if (status != 200 && status != 206) {
            if (t.error_body.size() + bytes > 65536) return 0;
            t.error_body.append(data, bytes);
            return bytes;
        }
        if (!t.started) {
            if (status == 206) {
                const auto expected = "bytes " + std::to_string(t.offset) + "-" +
                    std::to_string(t.expected - 1) + "/" + std::to_string(t.expected);
                if (t.offset == 0 || t.content_range != expected) {
                    t.error = "The server returned an unexpected download range. Retry the download.";
                    return 0;
                }
            } else t.offset = 0; // If-Range did not match: replace, never append.
            t.output.open(t.path, std::ios::binary | (status == 206 ? std::ios::app : std::ios::trunc));
            if (!t.output) { t.error = "Cannot write the temporary download."; return 0; }
            t.started = true;
        }
        if (bytes > std::uint64_t(t.expected - t.offset - t.received)) {
            t.error = "The file changed during the download. Retry it.";
            return 0;
        }
        t.output.write(data, std::streamsize(bytes));
        if (!t.output) { t.error = "Cannot write the temporary download. Check available disk space."; return 0; }
        t.received += std::int64_t(bytes);
        return bytes;
    } catch (...) { t.error = "Cannot save the download."; return 0; }
}
int progress(void* target, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& t = *static_cast<Transfer*>(target);
    try { return (*t.progress)(t.offset + t.received, t.expected) ? 0 : 1; }
    catch (...) { return 1; }
}
}

FilesApi::FilesApi(std::string server, std::string token) : api_(std::move(server), token), token_(std::move(token)) {}
void FilesApi::discover() {
    const char* unsupported = "This server does not support this version of file browsing. Update LOKI Server.";
    try {
        const auto o = json::parse(api_.ok("GET", "/files/v1"));
        if (o.value("protocol", 0) != 1 || o.value("checksum", "") != "sha256")
            throw std::runtime_error(unsupported);
    } catch (const json::exception&) { throw std::runtime_error(unsupported); }
    catch (const ApiError& e) { if (e.status == 404) throw std::runtime_error(unsupported); throw; }
}
Session FilesApi::login(const std::string& username, const std::string& password, const std::string& code,
                       const std::string& device, const std::string& platform) {
    const auto payload = json{{"username",username},{"password",password},{"code",code},{"name",device},{"platform",platform}}.dump();
    const auto o = json::parse(api_.ok("POST", "/auth/v1/login", &payload));
    const auto& u = o.at("user");
    return {o.at("token"),o.at("device").at("id"),u.at("username"),u.value("display_name",u.at("username").get<std::string>()),o.value("access",json::object()).value("sync",true)};
}
void FilesApi::sign_out() { api_.ok("DELETE", "/auth/v1/device"); }
std::vector<Share> FilesApi::shares() {
    std::vector<Share> out;
    const auto o = json::parse(api_.ok("GET", "/files/v1/shares"));
    for (const auto& s : o.at("shares")) out.push_back({s.at("id"),s.at("name")});
    return out;
}
ShareListing FilesApi::entries(const std::string& share, const std::string& path, int page) {
    const auto o = json::parse(api_.ok("GET", route(share,"entries",path) + "&page=" + std::to_string(page)));
    ShareListing out{{},o.at("total"),o.at("page")};
    for (const auto& e : o.at("entries")) out.entries.push_back({e.at("name"),e.at("directory"),e.at("size"),e.at("modified")});
    return out;
}
FileMetadata FilesApi::metadata(const std::string& share, const std::string& path) {
    const auto o = json::parse(api_.ok("GET", route(share,"metadata",path) + "&checksum=true"));
    FileMetadata out{o.at("size"),o.at("sha256"),o.value("last_modified",json()).is_null() ? "" : o.at("last_modified").get<std::string>()};
    if (out.size < 0 || out.sha256.size() != 64 || out.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("The server returned invalid file metadata.");
    return out;
}
fs::path FilesApi::download(const std::string& share, const std::string& path, const fs::path& cache,
                         const std::function<bool(std::int64_t,std::int64_t)>& report) {
    if (!report(0, 0)) throw std::runtime_error("Download cancelled.");
    const auto meta = metadata(share,path);
    if (!report(0,meta.size)) throw std::runtime_error("Download cancelled.");
    fs::create_directories(cache);
    // A partial belongs to one server, share, path AND content version.
    const auto key = Sha256::of(api_.base() + '\0' + share + '\0' + path + '\0' + meta.sha256);
    const auto temporary = cache / (key + ".part");
    auto offset = fs::exists(temporary) ? std::int64_t(fs::file_size(temporary)) : 0;
    if (offset > meta.size || (offset == meta.size && fs::exists(temporary) && Sha256::of_file(temporary) != meta.sha256)) {
        fs::resize_file(temporary,0); offset = 0;
    }
    if (offset != meta.size || !fs::exists(temporary)) {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
        if (!curl) throw std::runtime_error("Could not start the download.");
        auto* h = curl.get();
        const auto url = api_.base() + route(share,"file",path);
        curl_slist* raw = curl_slist_append(nullptr,("Authorization: Bearer " + token_).c_str());
        const auto range = "Range: bytes=" + std::to_string(offset) + "-";
        if (offset > 0) {
            raw = curl_slist_append(raw,range.c_str());
            if (!meta.last_modified.empty()) raw = curl_slist_append(raw,("If-Range: " + meta.last_modified).c_str());
        }
        std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> list(raw,curl_slist_free_all);
        Transfer t{h,temporary,{},"","","",offset,meta.size,0,false,&report};
        curl_easy_setopt(h,CURLOPT_URL,url.c_str());
        curl_easy_setopt(h,CURLOPT_HTTPHEADER,raw);
        curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,15L);
        curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(h,CURLOPT_LOW_SPEED_LIMIT,1L);
        curl_easy_setopt(h,CURLOPT_LOW_SPEED_TIME,90L);
        curl_easy_setopt(h,CURLOPT_HEADERFUNCTION,headers);
        curl_easy_setopt(h,CURLOPT_HEADERDATA,&t);
        curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,body);
        curl_easy_setopt(h,CURLOPT_WRITEDATA,&t);
        curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,progress);
        curl_easy_setopt(h,CURLOPT_XFERINFODATA,&t);
        const auto result = curl_easy_perform(h);
        long status = 0;
        curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
        if (t.output.is_open()) { t.output.flush(); if (!t.output) t.error = "Cannot finish writing the download."; t.output.close(); }
        if (!t.error.empty()) throw std::runtime_error(t.error);
        if (status != 200 && status != 206 && status != 0) Api::fail({status,t.error_body});
        if (result == CURLE_ABORTED_BY_CALLBACK) throw std::runtime_error("Download cancelled.");
        if (result != CURLE_OK) throw std::runtime_error(std::string("Download interrupted: ") + curl_easy_strerror(result) + ". Retry to resume.");
        // curl never calls the body callback for an empty file.
        if (meta.size == 0) { std::ofstream empty(temporary,std::ios::binary|std::ios::trunc); if (!empty) throw std::runtime_error("Cannot save the empty file."); }
    }
    if (!report(meta.size,meta.size)) throw std::runtime_error("Download cancelled.");
    if (fs::file_size(temporary) != std::uint64_t(meta.size) || Sha256::of_file(temporary) != meta.sha256) {
        fs::remove(temporary);
        throw std::runtime_error("The downloaded file failed verification. Retry to download a fresh copy.");
    }
    return temporary;
}

FileStore::FileStore(const fs::path& directory) : file_(directory / "files.json") {
    const auto o = json::parse(read_text(file_),nullptr,false);
    if (!o.is_object()) return;
    session_ = {o.value("server",""),{o.value("token",""),o.value("device_id",""),o.value("username",""),o.value("display_name","")}};
}
void FileStore::save(const std::string& server, const Session& session) {
    const auto normalized = Api::normalize(server);
    write_atomically(file_,json{{"server",normalized},{"token",session.token},{"device_id",session.device_id},{"username",session.username},{"display_name",session.display_name}}.dump(2));
    session_ = {normalized,session};
}
void FileStore::clear() { write_atomically(file_,"{}"); session_ = {}; }
} // namespace loki
