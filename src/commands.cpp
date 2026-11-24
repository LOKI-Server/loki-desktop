#include "commands.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include "management_api.hpp"
#ifdef LOKI_HAS_SKALD
#include <skald_vcs.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

using nlohmann::json;
namespace fs = std::filesystem;

namespace loki {
namespace {
fs::path worktree_link(Store& store, const std::string& id) {
    ManagementApi::repo_path(id);
    const auto account = store.account();
    return store.directory() / "skald-worktrees" /
        (Sha256::of(account.server + '\0' + account.session.username + '\0' + id) + ".json");
}

#ifdef LOKI_HAS_SKALD
using SkaldHandle = std::unique_ptr<SkaldRepo, decltype(&skald_repo_free)>;

void check_skald(int32_t code) {
    if (code == SKALD_OK) return;
    const auto* message = skald_last_error();
    throw std::runtime_error(message && *message ? message : "Skald could not complete the operation.");
}

void check_abi() {
    if (skald_abi_version() != 1) throw std::runtime_error("This build requires Skald C ABI 1.");
}

class SkaldOutput {
public:
    SkaldOutput() = default;
    ~SkaldOutput() { skald_string_free(value_); }
    SkaldOutput(const SkaldOutput&) = delete;
    SkaldOutput& operator=(const SkaldOutput&) = delete;
    char** out() { return &value_; }
    std::string text() const {
        if (!value_) throw std::runtime_error("Skald returned no result.");
        return value_;
    }
    json parsed() const { return json::parse(text()); }
private:
    char* value_ = nullptr;
};

fs::path workspace_root(const fs::path& directory) {
    auto path = fs::canonical(directory);
    while (!path.empty()) {
        if (fs::is_directory(path / ".skald")) return path;
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    throw std::runtime_error("Choose a Skald working folder containing .skald.");
}

SkaldHandle open_workspace(const fs::path& directory) {
    check_abi();
    const auto path = path_utf8(workspace_root(directory));
    SkaldRepo* handle = nullptr;
    check_skald(skald_open(path.c_str(), &handle));
    return SkaldHandle(handle, skald_repo_free);
}

SkaldHandle clone_workspace(const fs::path& destination, const std::string& url, const std::string& token) {
    check_abi();
    SkaldRepo* handle = nullptr;
    check_skald(skald_clone(path_utf8(destination).c_str(), url.c_str(), token.c_str(), &handle));
    return SkaldHandle(handle, skald_repo_free);
}

json status(const SkaldHandle& workspace) {
    SkaldOutput output;
    check_skald(skald_status(workspace.get(), output.out()));
    return output.parsed();
}

json remotes(const SkaldHandle& workspace) {
    SkaldOutput output;
    check_skald(skald_remotes(workspace.get(), output.out()));
    return output.parsed();
}

std::string current_branch(const SkaldHandle& workspace) {
    const auto info = status(workspace);
    if (info.at("branch").is_null()) throw std::runtime_error("Switch to a branch before pushing.");
    return info.at("branch");
}

json local_branches(const SkaldHandle& workspace) {
    SkaldOutput output;
    check_skald(skald_branches(workspace.get(), output.out()));
    return output.parsed();
}

std::vector<RepositoryCommit> history(const SkaldHandle& workspace, const char* revision, uint32_t limit) {
    SkaldOutput output;
    check_skald(skald_history(workspace.get(), revision, limit, output.out()));
    std::vector<RepositoryCommit> result;
    for (const auto& entry : output.parsed()) {
        const auto& data = entry.at(1);
        result.push_back({entry.at(0), data.at("root"), data.at("author"), data.at("message"),
                          data.at("timestamp"), data.at("parents")});
    }
    return result;
}

constexpr uint32_t comparison_history_limit = 1001;

BranchSync branch_sync(const SkaldHandle& workspace, const std::string& local, const std::string& remote) {
    if (local == remote) return BranchSync::UpToDate;
    const auto local_history = history(workspace, local.c_str(), comparison_history_limit);
    if (std::ranges::find(local_history, remote, &RepositoryCommit::id) != local_history.end())
        return BranchSync::Ahead;
    const auto remote_history = history(workspace, remote.c_str(), comparison_history_limit);
    if (std::ranges::find(remote_history, local, &RepositoryCommit::id) != remote_history.end())
        return BranchSync::Behind;
    return local_history.size() < comparison_history_limit && remote_history.size() < comparison_history_limit
        ? BranchSync::Diverged : BranchSync::Unknown;
}

std::vector<RepositoryDiff> differences(const SkaldHandle& workspace, const char* from, const char* to, bool staged) {
    SkaldOutput output;
    check_skald(skald_diff(workspace.get(), from, to, staged, output.out()));
    std::vector<RepositoryDiff> result;
    for (const auto& entry : output.parsed()) {
        RepositoryDiff difference{entry.at("path"), entry.at("change"), std::nullopt};
        if (!entry.at("text").is_null()) difference.patch = entry.at("text").get<std::string>();
        result.push_back(std::move(difference));
    }
    return result;
}

// Skald's diff API accepts revisions but has no empty-tree operand. For a root
// commit, list the files directly from the SDK's documented v1 tree objects.
void initial_files(const fs::path& root, const std::string& tree, const std::string& prefix,
                   unsigned depth, size_t& remaining, std::vector<RepositoryDiff>& result) {
    if (depth > 64 || tree.size() != 65 || tree.front() != 't' ||
        tree.find_first_not_of("0123456789abcdef", 1) != std::string::npos)
        throw std::runtime_error("Invalid Skald tree.");
    const auto path = root / ".skald" / "objects" / tree.substr(0, 3) / tree.substr(3);
    const auto length = fs::file_size(path);
    if (length < 2 || length > 8 * 1024 * 1024 + 1) throw std::runtime_error("Invalid Skald tree size.");
    std::ifstream file(path, std::ios::binary);
    if (file.get() != 0) throw std::runtime_error("Unsupported Skald tree codec.");
    std::string payload(size_t(length) - 1, '\0');
    if (!file.read(payload.data(), std::streamsize(payload.size()))) throw std::runtime_error("Cannot read Skald tree.");
    const auto entries = json::parse(payload);
    for (const auto& [name, entry] : entries.items()) {
        if (remaining == 0 || prefix.size() + name.size() > 1024) throw std::runtime_error("Skald tree exceeds format limits.");
        --remaining;
        const auto id = entry.at("id").get<std::string>();
        if (id.starts_with('t')) initial_files(root, id, prefix + name + "/", depth + 1, remaining, result);
        else if (id.starts_with('f')) result.push_back({prefix + name, "added", std::nullopt});
        else throw std::runtime_error("Invalid Skald tree entry.");
    }
}

std::vector<RepositoryDiff> initial_diff(const fs::path& root, const std::string& tree) {
    std::vector<RepositoryDiff> result;
    size_t remaining = 250000;
    initial_files(root, tree, "", 0, remaining, result);
    return result;
}

void commit_staged(const SkaldHandle& workspace, const std::string& summary, const std::string& author) {
    SkaldOutput result;
    check_skald(skald_commit(workspace.get(), author.c_str(), summary.c_str(), result.out()));
}

std::vector<std::string> merge_conflicts(int32_t code, const SkaldOutput& result) {
    if (code != SKALD_CONFLICT) {
        check_skald(code);
        return {};
    }
    return result.parsed().at("conflicts").get<std::vector<std::string>>();
}

void require_local_branch(const SkaldHandle& workspace, const std::string& branch) {
    const auto branches = local_branches(workspace);
    if (!branches.contains(branch) || branches.at(branch).is_null())
        throw std::runtime_error("The local Skald branch " + branch + " needs a commit before it can be pushed.");
}

void push_workspace(const SkaldHandle& workspace, const std::string& remote,
                    const std::string& token, const std::string& branch) {
    const auto code = skald_push(workspace.get(), remote.c_str(), token.c_str(), branch.c_str(), 0, 0);
    if (code != SKALD_OK) {
        const auto* error = skald_last_error();
        if (error && std::string_view(error) == "remote returned 507 Insufficient Storage") {
            throw std::runtime_error(
                "The server refused the push (507 Insufficient Storage). Your local commit is safe. "
                "Ask the server owner to check the repository and total quotas under Server settings > Skald storage, "
                "and check free disk space on the server. The repository object limit can also cause this error. "
                "You can retry Push after the storage issue is resolved.");
        }
        check_skald(code);
    }
}

std::string repository_url(const ManagementApi& api, const json& repo) {
    const auto id = repo.at("id").get<std::string>();
    ManagementApi::repo_path(id);
    const auto expected = api.server() + "/skald/v1/repos/" + id;
    if (repo.value("remote_url", std::string()) != expected)
        throw std::runtime_error("The repository does not advertise a Skald URL on the paired server. Use Loki 0.8.0 or newer and pair at its public address.");
    return expected;
}

std::string transfer_token(ManagementApi& api, const json& repo, const std::string& url, bool write_access) {
    const auto response = api.request("POST", ManagementApi::repo_path(repo.at("id")) + "/credential",
                                      {{"write", write_access}});
    if (response.at("remote_url") != url)
        throw std::runtime_error("The transfer credential does not match the repository URL.");
    const auto token = response.at("token").get<std::string>();
    if (token.empty()) throw std::runtime_error("Loki did not return a Skald transfer token.");
    return token;
}

std::string remote_for(const SkaldHandle& workspace, const std::string& url) {
    const auto known = remotes(workspace);
    if (known.value("origin", std::string()) == url) return "origin";
    for (const auto& [name, remote] : known.items())
        if (remote == url) return name;
    if (known.contains("loki"))
        throw std::runtime_error("The local 'loki' remote points to another repository. Configure a remote for the selected repository first.");
    check_skald(skald_remote(workspace.get(), "loki", url.c_str()));
    return "loki";
}

std::string repository_author(ManagementApi& api) {
    const auto who = api.request("GET", "/api/v1/me").at("user");
    const auto display = who.value("display_name", std::string());
    return display.empty() ? who.at("username").get<std::string>() : display;
}

#else
[[noreturn]] void skald_unavailable() {
    throw std::runtime_error("Skald working copies and transfers require the Windows x64 build of LOKI Desktop.");
}
#endif
} // namespace

HostedRepositories hosted_repositories(Store& store) {
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    return read_hosted_repositories(api.request("GET", "/api/v1/repos"), store.session().username);
}

HostedRepositories read_hosted_repositories(const json& listing, const std::string& username) {
    HostedRepositories result;
    for (const auto& repo : listing.at("repositories")) {
        if (!repo.contains("remote_url")) continue;
        std::string owner;
        if (const auto found = repo.find("owner"); found != repo.end() && found->is_object())
            owner = found->value("name", "");
        result.repositories.push_back({repo.at("id"), repo.at("name"), repo.at("role"), owner});
    }
    // Servers before LOKI 0.9 send only `create`, and only personal repositories exist there.
    if (listing.value("create", true)) result.owners.push_back({"", "You (@" + username + ")"});
    if (const auto groups = listing.find("groups"); groups != listing.end() && groups->is_array())
        for (const auto& group : *groups)
            result.owners.push_back({group.at("id"), "Group: " + group.value("name", "")});
    if (const auto transfers = listing.find("transfers"); transfers != listing.end() && transfers->is_array())
        for (const auto& transfer : *transfers)
            if (transfer.value("can_decide", false)) ++result.transfers_waiting;
    return result;
}

std::string create_hosted_repository(Store& store, const std::string& name, const std::optional<fs::path>& folder,
                                     const std::string& group) {
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    if (!folder) return api.create_repository(name, "main", group).at("id");
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(*folder);
    const auto branch = current_branch(workspace);
    require_local_branch(workspace, branch);
    if (remotes(workspace).contains("loki"))
        throw std::runtime_error("This working copy already has a 'loki' remote. Choose a working copy without that remote.");
    const auto repo = api.create_repository(name, branch, group);
    const auto id = repo.at("id").get<std::string>();
    link_repository_folder(store, id, *folder);
    try {
        const auto url = repository_url(api, repo);
        const auto token = transfer_token(api, repo, url, true);
        check_skald(skald_remote(workspace.get(), "loki", url.c_str()));
        push_workspace(workspace, "loki", token, branch);
    } catch (const std::exception& error) {
        throw std::runtime_error("Repository created. Refresh Repositories, select it, and retry Push. " + std::string(error.what()));
    }
    return id;
#else
    skald_unavailable();
#endif
}

void clone_hosted_repository(Store& store, const std::string& id, const fs::path& folder) {
#ifdef LOKI_HAS_SKALD
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    const auto repo = api.request("GET", ManagementApi::repo_path(id));
    const auto url = repository_url(api, repo);
    clone_workspace(folder, url, transfer_token(api, repo, url, false));
    link_repository_folder(store, id, folder);
#else
    skald_unavailable();
#endif
}

std::vector<std::string> transfer_repository(Store& store, const std::string& id, const fs::path& folder,
                                             RepositoryTransfer transfer) {
#ifdef LOKI_HAS_SKALD
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    const auto workspace = open_workspace(folder);
    const auto repo = api.request("GET", ManagementApi::repo_path(id));
    const auto url = repository_url(api, repo);
    const auto token = transfer_token(api, repo, url, transfer == RepositoryTransfer::Push);
    const auto remote = remote_for(workspace, url);
    if (transfer == RepositoryTransfer::Push) {
        const auto branch = current_branch(workspace);
        require_local_branch(workspace, branch);
        push_workspace(workspace, remote, token, branch);
    } else if (transfer == RepositoryTransfer::Fetch) {
        SkaldOutput result;
        check_skald(skald_fetch(workspace.get(), remote.c_str(), token.c_str(), result.out()));
    } else {
        const auto author = repository_author(api);
        SkaldOutput result;
        const auto code = skald_pull(workspace.get(), remote.c_str(), token.c_str(), nullptr, author.c_str(), result.out());
        return merge_conflicts(code, result);
    }
    return {};
#else
    skald_unavailable();
#endif
}

fs::path repository_folder(Store& store, const std::string& id) {
    const auto link = json::parse(read_text(worktree_link(store, id)), nullptr, false);
    return link.is_object() ? path_from_utf8(link.value("folder", "")) : fs::path{};
}

void link_repository_folder(Store& store, const std::string& id, const fs::path& folder) {
#ifdef LOKI_HAS_SKALD
    const auto root = workspace_root(folder);
    open_workspace(root);
    write_atomically(worktree_link(store, id), json{{"folder", path_utf8(root)}}.dump());
#else
    skald_unavailable();
#endif
}

RepositoryStatus repository_status(const fs::path& folder, const std::string& remote_url) {
#ifdef LOKI_HAS_SKALD
    const auto root = workspace_root(folder);
    const auto workspace = open_workspace(root);
    const auto info = status(workspace);
    const auto branches = local_branches(workspace);
    // The SDK exposes fetch results but has no reader for saved remote-tracking refs
    // or the pending merge. The v1 state file is atomically replaced by Skald.
    const auto state = json::parse(read_text(root / ".skald" / "state.json"));
    RepositoryStatus result;
    result.merging = !state.at("merge_parent").is_null();
    if (!info.at("revision").is_null()) result.revision = info.at("revision").get<std::string>();
    for (const auto& [name, revision] : branches.items()) {
        RepositoryBranch branch{name, std::nullopt};
        if (!revision.is_null()) branch.revision = revision.get<std::string>();
        result.branches.push_back(std::move(branch));
    }

    std::ostringstream output;
    if (!info.at("branch").is_null()) result.current_branch = info.at("branch").get<std::string>();
    for (const auto& [path, change] : info.at("staged").items()) {
        result.staged.push_back(path);
        output << "staged   " << change.get<std::string>() << "  " << path << '\n';
    }
    for (const auto& [path, change] : info.at("unstaged").items()) {
        result.unstaged.push_back(path);
        output << "working  " << change.get<std::string>() << "  " << path << '\n';
    }
    result.conflicts = info.at("conflicts").get<std::vector<std::string>>();
    if (result.merging) output << "Merge in progress. Stage each resolution and commit the merge.\n";
    for (const auto& path : result.conflicts) output << "conflict " << path << '\n';
    if (!result.merging && result.staged.empty() && result.unstaged.empty() && result.conflicts.empty())
        output << "Working copy is clean.\n";
    result.changes = output.str();

    if (remote_url.empty()) return result;
    const auto& configured = state.at("remotes");
    const auto& fetched = state.at("fetched");
    std::string remote;
    if (configured.value("origin", std::string()) == remote_url) remote = "origin";
    else for (const auto& [name, url] : configured.items()) {
        if (url == remote_url) { remote = name; break; }
    }
    if (!fetched.contains(remote)) return result;
    for (const auto& [ref, revision] : fetched.at(remote).items()) {
        constexpr std::string_view prefix = "branches/";
        if (ref.starts_with(prefix)) result.remote_branches.push_back({ref.substr(prefix.size()), revision.get<std::string>()});
    }
    if (!result.current_branch) return result;
    const auto found = std::ranges::find(result.remote_branches, *result.current_branch, &RepositoryBranch::name);
    if (found == result.remote_branches.end()) result.sync = BranchSync::LocalOnly;
    else if (!result.revision) result.sync = BranchSync::Behind;
    else result.sync = branch_sync(workspace, *result.revision, found->revision.value());
    return result;
#else
    skald_unavailable();
#endif
}

RepositoryStatus hosted_repository_status(Store& store, const std::string& id, const fs::path& folder) {
    ManagementApi::repo_path(id);
    const auto origin = ManagementApi::origin(store.account().server);
    return repository_status(folder, origin + "/skald/v1/repos/" + id);
}

void create_repository_branch(const fs::path& folder, const std::string& name, const std::string& from) {
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(folder);
    check_skald(skald_branch(workspace.get(), name.c_str(), from.empty() ? nullptr : from.c_str()));
#else
    skald_unavailable();
#endif
}

void switch_repository_branch(const fs::path& folder, const std::string& name) {
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(folder);
    check_skald(skald_checkout(workspace.get(), name.c_str(), 0));
#else
    skald_unavailable();
#endif
}

void delete_repository_branch(const fs::path& folder, const std::string& name) {
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(folder);
    check_skald(skald_branch_delete(workspace.get(), name.c_str(), 0));
#else
    skald_unavailable();
#endif
}

void commit_repository_changes(const fs::path& folder, const std::string& summary, const std::string& author) {
#ifdef LOKI_HAS_SKALD
    if (summary.empty()) throw std::runtime_error("Enter a commit summary.");
    if (author.empty()) throw std::runtime_error("Sign in to LOKI to choose the commit author.");
    const auto workspace = open_workspace(folder);
    check_skald(skald_stage(workspace.get(), "."));
    commit_staged(workspace, summary, author);
#else
    skald_unavailable();
#endif
}

void stage_repository_paths(const fs::path& folder, const std::vector<std::string>& paths, bool stage) {
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(folder);
    // The SDK accepts a single path, so successful paths remain staged if a later path fails.
    for (const auto& path : paths) {
        const auto code = stage ? skald_stage(workspace.get(), path.c_str()) : skald_unstage(workspace.get(), path.c_str());
        try { check_skald(code); }
        catch (const std::runtime_error& error) { throw std::runtime_error(path + ": " + error.what()); }
    }
#else
    skald_unavailable();
#endif
}

void commit_staged_repository_changes(const fs::path& folder, const std::string& summary, const std::string& author) {
#ifdef LOKI_HAS_SKALD
    if (summary.empty()) throw std::runtime_error("Enter a commit summary.");
    if (author.empty()) throw std::runtime_error("Sign in to LOKI to choose the commit author.");
    commit_staged(open_workspace(folder), summary, author);
#else
    skald_unavailable();
#endif
}

std::vector<RepositoryCommit> repository_history(const fs::path& folder) {
#ifdef LOKI_HAS_SKALD
    return history(open_workspace(folder), nullptr, 100);
#else
    skald_unavailable();
#endif
}

std::vector<RepositoryDiff> repository_working_diff(const fs::path& folder, bool staged) {
#ifdef LOKI_HAS_SKALD
    return differences(open_workspace(folder), nullptr, nullptr, staged);
#else
    skald_unavailable();
#endif
}

std::vector<RepositoryDiff> repository_commit_diff(const fs::path& folder, const RepositoryCommit& commit) {
#ifdef LOKI_HAS_SKALD
    if (commit.parents.empty()) return initial_diff(workspace_root(folder), commit.root);
    return differences(open_workspace(folder), commit.parents.front().c_str(), commit.id.c_str(), false);
#else
    skald_unavailable();
#endif
}

BranchComparison compare_repository_branch(const fs::path& folder, const std::optional<std::string>& current,
                                           const std::string& target) {
#ifdef LOKI_HAS_SKALD
    BranchComparison result;
    if (current == target) return result;
    const auto workspace = open_workspace(folder);
    auto theirs = history(workspace, target.c_str(), comparison_history_limit);
    if (!current) {
        if (theirs.empty()) throw std::runtime_error("The comparison branch has no commits.");
        result.files = initial_diff(workspace_root(folder), theirs.front().root);
        result.history_limited = theirs.size() == comparison_history_limit;
        result.incoming = std::move(theirs);
        return result;
    }

    result.files = differences(workspace, current->c_str(), target.c_str(), false);
    auto ours = history(workspace, current->c_str(), comparison_history_limit);
    result.history_limited = ours.size() == comparison_history_limit || theirs.size() == comparison_history_limit;
    if (!result.history_limited) {
        std::unordered_set<std::string> our_ids, their_ids;
        for (const auto& commit : ours) our_ids.insert(commit.id);
        for (const auto& commit : theirs) their_ids.insert(commit.id);
        std::erase_if(ours, [&](const RepositoryCommit& commit) { return their_ids.contains(commit.id); });
        std::erase_if(theirs, [&](const RepositoryCommit& commit) { return our_ids.contains(commit.id); });
        result.current_only = std::move(ours);
        result.incoming = std::move(theirs);
    }
    return result;
#else
    skald_unavailable();
#endif
}

std::vector<std::string> merge_repository_branch(const fs::path& folder, const std::optional<std::string>& branch,
                                                const std::optional<std::string>& current,
                                                const std::string& target, const std::string& author,
                                                const std::string& summary) {
#ifdef LOKI_HAS_SKALD
    if (!branch) throw std::runtime_error("Switch to a local branch before merging.");
    if (author.empty()) throw std::runtime_error("Sign in to LOKI to choose the commit author.");
    const auto workspace = open_workspace(folder);
    const auto info = status(workspace);
    if (info.at("branch") != *branch ||
        (current ? info.at("revision") != *current : !info.at("revision").is_null()))
        throw std::runtime_error("The current branch changed. Compare the branches again before merging.");
    SkaldOutput result;
    const auto code = skald_merge(workspace.get(), target.c_str(), author.c_str(), summary.c_str(), result.out());
    return merge_conflicts(code, result);
#else
    skald_unavailable();
#endif
}

void abort_repository_merge(const fs::path& folder) {
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(folder);
    check_skald(skald_merge_abort(workspace.get()));
#else
    skald_unavailable();
#endif
}

bool starts_at_sign_in() {
#ifdef _WIN32
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"LOKI Client", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
#else
    return false;
#endif
}

void set_start_at_sign_in(bool enabled) {
#ifdef _WIN32
    constexpr auto key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto entry = L"LOKI Client";
    if (!enabled) {
        const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, entry);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) throw std::runtime_error("Cannot disable start at sign-in.");
        return;
    }
    std::wstring current(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, current.data(), DWORD(current.size()));
    if (!size || size == current.size()) throw std::runtime_error("Cannot locate the desktop executable.");
    current.resize(size);
    const auto gui = fs::path(current).parent_path() / "loki-client.exe";
    if (!fs::is_regular_file(gui)) throw std::runtime_error("Cannot find loki-client.exe to start it at sign-in.");
    const auto command = L"\"" + gui.native() + L"\" --background";
    if (RegSetKeyValueW(HKEY_CURRENT_USER, key, entry, REG_SZ, command.c_str(),
                        DWORD((command.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot enable start at sign-in.");
#else
    (void)enabled;
    throw std::runtime_error("Start at sign-in is available on Windows.");
#endif
}


}
