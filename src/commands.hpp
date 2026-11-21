#pragma once
#include "store.hpp"

namespace loki {
struct HostedRepository { std::string id, name, role; };
enum class RepositoryTransfer { Fetch, Pull, Push };
std::vector<HostedRepository> hosted_repositories(Store& store);
std::string create_hosted_repository(Store& store, const std::string& name,
                                     const std::optional<std::filesystem::path>& folder);
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
