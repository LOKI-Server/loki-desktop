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
enum class BranchSync { Unknown, LocalOnly, UpToDate, Ahead, Behind, Diverged };
struct RepositoryBranch {
    std::string name;
    std::optional<std::string> revision;
};
struct RepositoryStatus {
    std::optional<std::string> current_branch;
    std::optional<std::string> revision;
    std::vector<RepositoryBranch> branches, remote_branches;
    std::vector<std::string> staged, unstaged, conflicts;
    BranchSync sync = BranchSync::Unknown;
    bool merging = false;
    std::string changes;
};
struct RepositoryCommit {
    std::string id, root, author, message;
    std::int64_t timestamp;
    std::vector<std::string> parents;
};
struct RepositoryDiff {
    std::string path, change;
    std::optional<std::string> patch;
};
struct BranchComparison {
    std::vector<RepositoryCommit> current_only, incoming;
    std::vector<RepositoryDiff> files;
    bool history_limited = false;
};
struct RepositoryFolder {
    std::filesystem::path root;
    bool initialized = false;
    bool can_rename_ignore = false;
};
HostedRepositories hosted_repositories(Store& store);
// Reads a GET /api/v1/repos response from any LOKI version.
HostedRepositories read_hosted_repositories(const nlohmann::json& listing, const std::string& username);
std::string create_hosted_repository(Store& store, const std::string& name,
                                     const std::optional<std::filesystem::path>& folder,
                                     const std::string& group = {});
void clone_hosted_repository(Store& store, const std::string& id, const std::filesystem::path& folder);
std::vector<std::string> transfer_repository(Store& store, const std::string& id,
                                             const std::filesystem::path& folder, RepositoryTransfer transfer);
std::filesystem::path repository_folder(Store& store, const std::string& id);
RepositoryFolder inspect_repository_folder(const std::filesystem::path& folder);
void initialize_repository_folder(const std::filesystem::path& folder);
void link_repository_folder(Store& store, const std::string& id, const std::filesystem::path& folder);
void rename_repository_ignore(const std::filesystem::path& folder);
RepositoryStatus repository_status(const std::filesystem::path& folder, const std::string& remote_url = {});
RepositoryStatus hosted_repository_status(Store& store, const std::string& id, const std::filesystem::path& folder);
void create_repository_branch(const std::filesystem::path& folder, const std::string& name, const std::string& from = {});
void switch_repository_branch(const std::filesystem::path& folder, const std::string& name);
void delete_repository_branch(const std::filesystem::path& folder, const std::string& name);
void commit_repository_changes(const std::filesystem::path& folder, const std::string& summary, const std::string& author);
void stage_repository_paths(const std::filesystem::path& folder, const std::vector<std::string>& paths, bool stage);
void commit_staged_repository_changes(const std::filesystem::path& folder, const std::string& summary, const std::string& author);
std::vector<RepositoryCommit> repository_history(const std::filesystem::path& folder);
std::vector<RepositoryDiff> repository_working_diff(const std::filesystem::path& folder, bool staged);
std::vector<RepositoryDiff> repository_commit_diff(const std::filesystem::path& folder, const RepositoryCommit& commit);
BranchComparison compare_repository_branch(const std::filesystem::path& folder,
                                           const std::optional<std::string>& current, const std::string& target);
std::vector<std::string> merge_repository_branch(const std::filesystem::path& folder,
                                                const std::optional<std::string>& branch,
                                                const std::optional<std::string>& current,
                                                const std::string& target, const std::string& author,
                                                const std::string& summary);
void abort_repository_merge(const std::filesystem::path& folder);
bool starts_at_sign_in();
void set_start_at_sign_in(bool enabled);
}
