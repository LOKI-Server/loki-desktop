#include "management_api.hpp"

#include <curl/curl.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>

using nlohmann::json;

namespace loki {
namespace {

void check_request(const std::string& method, const std::string& path) {
    if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE") {
        throw std::runtime_error("Choose GET, POST, PUT, PATCH, or DELETE.");
    }
    const std::string_view route = std::string_view(path).substr(0, path.find('?'));
    for (const std::string_view prefix : {"/api/v1", "/auth/v1", "/files/v1", "/sync/v1"}) {
        if ((route == prefix || (route.starts_with(prefix) && route[prefix.size()] == '/'))
            && route.find("..") == std::string_view::npos && path.find_first_of("\\#\r\n ") == std::string::npos) {
            return;
        }
    }
    throw std::runtime_error("Use a versioned LOKI API path, such as /api/v1/repos.");
}

}

ManagementApi::ManagementApi(std::string server, std::string token) : api_(origin(server), std::move(token)) {}

std::string ManagementApi::origin(const std::string& server) {
    static Api curl_initialized("http://localhost");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    auto part = [&](CURLUPart item) {
        char* raw = nullptr;
        const auto status = curl_url_get(url.get(), item, &raw, 0);
        const std::unique_ptr<char, decltype(&curl_free)> value(raw, curl_free);
        return status == CURLUE_OK ? std::string(raw) : std::string();
    };
    const auto normalized = Api::normalize(server);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, normalized.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK
        || part(CURLUPART_PATH) != "/" || normalized.find_first_of("?#\r\n") != std::string::npos) {
        throw std::runtime_error("Enter the LOKI server origin with no path, query, or sign-in details.");
    }
    const auto scheme = part(CURLUPART_SCHEME);
    const auto host = part(CURLUPART_HOST);
    if (host.empty() || (scheme != "https" && !(scheme == "http"
        && (host == "localhost" || host == "127.0.0.1" || host == "[::1]")))) {
        throw std::runtime_error("Use HTTPS for LOKI. Plain HTTP is available on loopback for local development.");
    }
    const auto port = part(CURLUPART_PORT);
    return scheme + "://" + host + (port.empty() ? "" : ":" + port);
}

std::string ManagementApi::repo_path(const std::string& id) {
    if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) {
        throw std::runtime_error("Invalid repository ID.");
    }
    return "/api/v1/repos/" + id;
}

json ManagementApi::request(const std::string& method, const std::string& path) {
    check_request(method, path);
    return json::parse(api_.ok(method, path));
}

json ManagementApi::request(const std::string& method, const std::string& path, const json& body) {
    check_request(method, path);
    const auto text = body.dump();
    return json::parse(api_.ok(method, path, &text));
}

namespace {
Session management_session(const json& data) {
    const auto& user = data.at("user");
    return {data.at("token"), data.at("device").at("id"), user.at("username"),
            user.at("display_name"), data.at("access").value("sync", false), data.at("scope")};
}
}

Session ManagementApi::login(const std::string& scope, const std::string& username, const std::string& password,
                             const std::string& code, const std::string& device, const std::string& platform) {
    return management_session(request("POST", "/api/v1/auth/login",
        {{"scope", scope}, {"username", username}, {"password", password}, {"code", code},
         {"name", device}, {"platform", platform}}));
}

Pairing ManagementApi::pair(const std::string& scope, const std::string& device, const std::string& platform) {
    const auto data = request("POST", "/api/v1/auth/pair",
                              {{"scope", scope}, {"name", device}, {"platform", platform}});
    return {data.at("device_code"), data.at("user_code"), data.at("verification_uri"),
            data.at("verification_uri_complete"), data.value("interval", 5), data.value("expires_in", 600)};
}

std::optional<Session> ManagementApi::poll(const std::string& scope, const std::string& device_code) {
    try {
        return management_session(request("POST", "/api/v1/auth/pair/poll", {{"scope", scope}, {"device_code", device_code}}));
    } catch (const ApiError& error) {
        if (error.code == "authorization_pending" || error.code == "slow_down") return std::nullopt;
        throw;
    }
}

void ManagementApi::sign_out() { api_.sign_out(); }

json ManagementApi::create_repository(const std::string& name, const std::string& branch) {
    return request("POST", "/api/v1/repos", {{"name", name}, {"default_branch", branch}});
}

}
