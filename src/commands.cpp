#include "commands.hpp"
#include "sha256.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
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

void require_local_branch(const SkaldHandle& workspace, const std::string& branch) {
    SkaldOutput output;
    check_skald(skald_branches(workspace.get(), output.out()));
    const auto branches = output.parsed();
    if (!branches.contains(branch) || branches.at(branch).is_null())
        throw std::runtime_error("The local Skald branch " + branch + " needs a commit before it can be pushed.");
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

std::vector<HostedRepository> hosted_repositories(Store& store) {
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    const auto listing = api.request("GET", "/api/v1/repos");
    std::vector<HostedRepository> repos;
    for (const auto& repo : listing.at("repositories")) {
        if (repo.contains("remote_url"))
            repos.push_back({repo.at("id"), repo.at("name"), repo.at("role")});
    }
    return repos;
}

std::string create_hosted_repository(Store& store, const std::string& name, const std::optional<fs::path>& folder) {
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    if (!folder) return api.create_repository(name, "main").at("id");
#ifdef LOKI_HAS_SKALD
    const auto workspace = open_workspace(*folder);
    const auto branch = current_branch(workspace);
    require_local_branch(workspace, branch);
    if (remotes(workspace).contains("loki"))
        throw std::runtime_error("This working copy already has a 'loki' remote. Choose a working copy without that remote.");
    const auto repo = api.create_repository(name, branch);
    const auto id = repo.at("id").get<std::string>();
    link_repository_folder(store, id, *folder);
    try {
        const auto url = repository_url(api, repo);
        const auto token = transfer_token(api, repo, url, true);
        check_skald(skald_remote(workspace.get(), "loki", url.c_str()));
        check_skald(skald_push(workspace.get(), "loki", token.c_str(), branch.c_str(), 0, 0));
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

void transfer_repository(Store& store, const std::string& id, const fs::path& folder,
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
        check_skald(skald_push(workspace.get(), remote.c_str(), token.c_str(), branch.c_str(), 0, 0));
    } else if (transfer == RepositoryTransfer::Fetch) {
        SkaldOutput result;
        check_skald(skald_fetch(workspace.get(), remote.c_str(), token.c_str(), result.out()));
    } else {
        const auto author = repository_author(api);
        SkaldOutput result;
        const auto code = skald_pull(workspace.get(), remote.c_str(), token.c_str(), nullptr, author.c_str(), result.out());
        if (code == SKALD_CONFLICT) {
            std::string message = "Resolve the Skald merge conflicts, then commit the changes:";
            const auto merge = result.parsed();
            for (const auto& file : merge.at("conflicts")) message += "\n" + file.get<std::string>();
            throw std::runtime_error(message);
        }
        check_skald(code);
    }
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

std::string repository_status(const fs::path& folder) {
#ifdef LOKI_HAS_SKALD
    const auto info = status(open_workspace(folder));
    std::ostringstream output;
    if (info.at("branch").is_null()) output << "Detached at " << info.at("revision").get<std::string>() << '\n';
    else output << "Branch " << info.at("branch").get<std::string>() << '\n';
    for (const auto& [path, change] : info.at("staged").items())
        output << "staged   " << change.get<std::string>() << "  " << path << '\n';
    for (const auto& [path, change] : info.at("unstaged").items())
        output << "working  " << change.get<std::string>() << "  " << path << '\n';
    for (const auto& path : info.at("conflicts")) output << "conflict " << path.get<std::string>() << '\n';
    if (info.at("staged").empty() && info.at("unstaged").empty() && info.at("conflicts").empty())
        output << "Working copy is clean.\n";
    return output.str();
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
    SkaldOutput result;
    check_skald(skald_commit(workspace.get(), author.c_str(), summary.c_str(), result.out()));
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
