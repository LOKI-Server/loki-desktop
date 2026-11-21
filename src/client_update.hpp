#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace loki {

struct ClientUpdate {
    std::string version, url, sha256;
    std::uint64_t size;
};

std::optional<ClientUpdate> select_client_update(std::string_view json, std::string_view current_version);

#ifdef _WIN32
std::optional<ClientUpdate> check_client_update(std::string_view current_version);

class PreparedClientUpdate {
public:
    ~PreparedClientUpdate();
    PreparedClientUpdate(const PreparedClientUpdate&) = delete;
    PreparedClientUpdate& operator=(const PreparedClientUpdate&) = delete;
    void install_after_exit();
private:
    PreparedClientUpdate(std::filesystem::path folder, std::string version);
    std::filesystem::path folder_;
    std::string version_;
    friend std::shared_ptr<PreparedClientUpdate> prepare_client_update(const ClientUpdate& update);
};

std::shared_ptr<PreparedClientUpdate> prepare_client_update(const ClientUpdate& update);
#endif

}  // namespace loki
