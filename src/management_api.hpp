#pragma once

#include "api.hpp"
#include <nlohmann/json_fwd.hpp>

namespace loki {

class ManagementApi {
public:
    explicit ManagementApi(std::string server, std::string token = {});
    const std::string& server() const { return api_.base(); }
    Pairing pair(const std::string& scope, const std::string& device = "LOKI Desktop", const std::string& platform = "desktop");
    Session login(const std::string& scope, const std::string& username, const std::string& password,
                  const std::string& code, const std::string& device = "LOKI Desktop", const std::string& platform = "desktop");
    std::optional<Session> poll(const std::string& scope, const std::string& device_code);
    void sign_out();
    nlohmann::json request(const std::string& method, const std::string& path);
    nlohmann::json request(const std::string& method, const std::string& path, const nlohmann::json& body);
    // `group`, when set, makes a group own the repository (LOKI 0.9 and later).
    nlohmann::json create_repository(const std::string& name, const std::string& branch, const std::string& group = {});
    static std::string origin(const std::string& server);
    static std::string repo_path(const std::string& id);

private:
    Api api_;
};

}
