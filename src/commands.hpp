#pragma once
#include "store.hpp"
#include <nlohmann/json_fwd.hpp>

namespace loki {
// `owner` is the person or group that owns the repository; servers before LOKI 0.9 leave it empty.
struct HostedRepository {
    std::string id, name, role, owner;
    std::string label() const { return owner.empty() ? name : owner + " / " + name; }
};
// Who a new repository can belong to. An empty `group` means the signed-in person.
struct RepositoryOwner { std::string group, label; };
struct HostedRepositories {
    std::vector<HostedRepository> repositories;
    std::vector<RepositoryOwner> owners;
    // Transfers the signed-in person may accept or decline in the LOKI web dashboard.
    size_t transfers_waiting = 0;
};
enum class RepositoryTransfer { Fetch, Pull, Push };
HostedRepositories hosted_repositories(Store& store);
// Reads a GET /api/v1/repos response from any LOKI version.
HostedRepositories read_hosted_repositories(const nlohmann::json& listing, const std::string& username);
std::string create_hosted_repository(Store& store, const std::string& name,
                                     const std::optional<std::filesystem::path>& folder,
                                     const std::string& group = {});
void clone_hosted_repository(Store& store, const std::string& id, const std::filesystem::path& folder);
void transfer_repository(Store& store, const std::string& id, const std::filesystem::path& folder,
                         RepositoryTransfer transfer);
std::filesystem::path repository_folder(Store& store, const std::string& id);
void link_repository_folder(Store& store, const std::string& id, const std::filesystem::path& folder);
std::string repository_status(const std::filesystem::path& folder);
void commit_repository_changes(const std::filesystem::path& folder, const std::string& summary, const std::string& author);
bool starts_at_sign_in();
void set_start_at_sign_in(bool enabled);
}
