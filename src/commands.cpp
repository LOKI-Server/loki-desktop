#include "commands.hpp"
#include "process.hpp"
#include "sha256.hpp"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

using nlohmann::json;
namespace fs = std::filesystem;

namespace loki {
namespace {
struct Args {
    std::vector<std::string> words;
    size_t position = 0;

    bool more() const { return position < words.size(); }
    std::string next(const std::string& label) {
        if (!more()) throw std::runtime_error("Expected " + label + ". See loki-git --help.");
        return words[position++];
    }
    void end() {
        if (more()) throw std::runtime_error("Unexpected argument " + next("argument") + ".");
    }
};

void write(std::ostream& out, const json& value) { out << value.dump(2) << '\n'; }

std::string base64(const std::string& text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < text.size(); i += 3) {
        const auto byte = [&](size_t j) { return j < text.size() ? unsigned(static_cast<unsigned char>(text[j])) : 0u; };
        const auto bits = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        result.push_back(alphabet[(bits >> 18) & 63]);
        result.push_back(alphabet[(bits >> 12) & 63]);
        result.push_back(i + 1 < text.size() ? alphabet[(bits >> 6) & 63] : '=');
        result.push_back(i + 2 < text.size() ? alphabet[bits & 63] : '=');
    }
    return result;
}

std::string clone_url(ManagementApi& api, const json& repo) {
    const auto id = repo.at("id").get<std::string>();
    ManagementApi::repo_path(id);
    const auto expected = api.server() + "/git-http/" + id + ".git";
    if (repo.at("clone_url") != expected) {
        throw std::runtime_error("The repository advertises a different server origin. Pair with that server address to transfer Git data.");
    }
    return expected;
}

loki::ProcessResult git(const std::vector<std::string>& command,
                        std::map<std::string, std::string> environment = {}, bool capture = false, bool errors = false) {
    environment["LOKI_TOKEN"] = "";
    return loki::run_process(command, environment, capture, errors);
}

int transfer(ManagementApi& api, const json& repo, const std::vector<std::string>& command, bool write, std::ostream* log) {
    const auto url = clone_url(api, repo);
    const auto credential = api.request("POST", ManagementApi::repo_path(repo.at("id")) + "/credential", {{"write", write}});
    const auto basic = base64(credential.at("username").get<std::string>() + ':' + credential.at("token").get<std::string>());
    const auto result = git(command, {
        {"GIT_CONFIG_COUNT", "4"},
        {"GIT_CONFIG_KEY_0", "http." + url + ".extraHeader"},
        {"GIT_CONFIG_VALUE_0", "Authorization: Basic " + basic},
        {"GIT_CONFIG_KEY_1", "http." + url + ".followRedirects"},
        {"GIT_CONFIG_VALUE_1", "false"},
        {"GIT_CONFIG_KEY_2", "lfs.url"},
        {"GIT_CONFIG_VALUE_2", url + "/info/lfs"},
        {"GIT_CONFIG_KEY_3", "lfs.pushurl"},
        {"GIT_CONFIG_VALUE_3", url + "/info/lfs"},
        {"GIT_TERMINAL_PROMPT", "0"},
    }, log != nullptr, log != nullptr);
    if (log) *log << result.output;
    return result.status;
}

std::string checked_branch(std::string branch) {
    if (branch.empty() || branch.front() == '-' || git({"git", "check-ref-format", "refs/heads/" + branch}).status != 0)
        throw std::runtime_error("Choose a valid branch name.");
    return branch;
}

std::string current_branch(const std::string& cwd) {
    auto current = git({"git", "-C", cwd, "symbolic-ref", "--quiet", "--short", "HEAD"}, {}, true);
    if (current.status || current.output.empty()) throw std::runtime_error("Use --branch when HEAD is detached.");
    current.output.erase(current.output.find_last_not_of("\r\n") + 1);
    return checked_branch(current.output);
}

int repository(ManagementApi& api, Args& args, const std::string& cwd, std::ostream& out, std::ostream* log) {
    const auto action = args.next("a repo command");
    if (action == "list") {
        args.end();
        write(out, api.request("GET", "/api/v1/repos"));
        return 0;
    }
    const auto name = args.next("a repository name or ID");
    if (action == "view") {
        args.end();
        write(out, api.repository(name));
        return 0;
    }
    if (action == "clone") {
        auto repo = api.repository(name);
        std::vector<std::string> command{"git", "-C", cwd, "clone", "--", clone_url(api, repo)};
        if (args.more()) command.push_back(args.next("a directory"));
        args.end();
        return transfer(api, repo, command, false, log);
    }
    if (action == "fetch" || action == "pull") {
        args.end();
        const auto repo = api.repository(name);
        const auto branch = current_branch(cwd);
        std::vector<std::string> command{"git", "-C", cwd, action};
        if (action == "pull") command.push_back("--ff-only");
        command.insert(command.end(), {"--", clone_url(api, repo), "refs/heads/" + branch});
        return transfer(api, repo, command, false, log);
    }
    if (action != "create" && action != "push") throw std::runtime_error("Unknown repo command.");
    bool push = action == "push";
    std::string branch, remote = "loki";
    while (args.more()) {
        const auto flag = args.next("an option");
        if (flag == "--branch") branch = checked_branch(args.next("a branch"));
        else if (flag == "--push" && action == "create") push = true;
        else if (flag == "--remote" && action == "create") remote = args.next("a remote name");
        else throw std::runtime_error("Unknown option " + flag + ".");
    }
    if (push && git({"git", "-C", cwd, "rev-parse", "--verify", "HEAD"}, {}, true).status != 0)
        throw std::runtime_error("Run this from a Git repository with at least one commit.");
    if (branch.empty()) branch = push ? current_branch(cwd) : "main";
    if (action == "push") {
        const auto repo = api.repository(name);
        return transfer(api, repo, {"git", "-C", cwd, "push", "--", clone_url(api, repo), "HEAD:refs/heads/" + branch}, true, log);
    }
    if (push && (remote.empty() || remote.front() == '-' ||
        git({"git", "check-ref-format", "refs/remotes/" + remote + "/branch"}).status != 0)) {
        throw std::runtime_error("Choose a valid remote name.");
    }
    if (push) {
        const auto remotes = git({"git", "-C", cwd, "remote"}, {}, true);
        if (remotes.status != 0) return remotes.status;
        std::istringstream names(remotes.output);
        for (std::string existing; names >> existing;) {
            if (existing == remote) throw std::runtime_error("Remote " + remote + " already exists. Choose --remote NAME.");
        }
    }
    const auto repo = api.create_repository(name, branch);
    write(out, repo);
    if (!push) return 0;
    const auto url = clone_url(api, repo);
    if (const auto added = git({"git", "-C", cwd, "remote", "add", remote, url}); added.status != 0) return added.status;
    const auto pushed = transfer(api, repo, {"git", "-C", cwd, "push", "--set-upstream", "--", remote, "HEAD:refs/heads/" + branch}, true, log);
    if (pushed) out << "The repository was created. Retry with loki-git repo push " << repo.at("id").get<std::string>() << ".\n";
    return pushed;
}

} // namespace

namespace {

fs::path worktree_link(Store& store, const std::string& id) {
    ManagementApi::repo_path(id);
    const auto account = store.account();
    return store.directory() / "git-worktrees" /
        (Sha256::of(account.server + '\0' + account.session.username + '\0' + id) + ".json");
}

std::string worktree_command(const fs::path& folder, std::vector<std::string> args) {
    std::vector<std::string> command{"git", "-C", path_utf8(folder)};
    command.insert(command.end(), args.begin(), args.end());
    const auto result = git(command, {{"GIT_TERMINAL_PROMPT", "0"}}, true, true);
    if (result.status) throw std::runtime_error(result.output.empty() ? "Git could not complete the operation." : result.output);
    return result.output;
}

}

std::vector<GitRepository> git_repositories(Store& store) {
    const auto account = store.account();
    ManagementApi api(account.server, account.session.token);
    const auto listing = api.request("GET", "/api/v1/repos");
    std::vector<GitRepository> repos;
    for (const auto& repo : listing.at("repositories"))
        repos.push_back({repo.at("id"), repo.at("name"), repo.at("role")});
    return repos;
}

fs::path git_folder(Store& store, const std::string& id) {
    const auto link = json::parse(read_text(worktree_link(store, id)), nullptr, false);
    return link.is_object() ? path_from_utf8(link.value("folder", "")) : fs::path{};
}

void link_git_folder(Store& store, const std::string& id, const fs::path& folder) {
    auto root = worktree_command(folder, {"rev-parse", "--show-toplevel"});
    const auto last = root.find_last_not_of("\r\n");
    if (last == std::string::npos) throw std::runtime_error("Choose a Git working folder.");
    root.resize(last + 1);
    write_atomically(worktree_link(store, id), json{{"folder", root}}.dump());
}

std::string git_status(const fs::path& folder) {
    return worktree_command(folder, {"-c", "core.quotepath=false", "status", "--short", "--branch"});
}

void commit_git_changes(const fs::path& folder, const std::string& summary) {
    if (summary.empty()) throw std::runtime_error("Enter a commit summary.");
    worktree_command(folder, {"add", "--all"});
    worktree_command(folder, {"commit", "-m", summary});
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

int git_command(Desktop& client, std::vector<std::string> words,
                const fs::path& cwd, std::ostream& out, const std::string& server, const std::string& token, std::ostream* git_log) {
    Args args{std::move(words)};
    const auto account = client.store().account();
    const auto credential = token.empty() ? account.session.token : token;
    if (credential.empty()) throw std::runtime_error("Sign in to LOKI first.");
    ManagementApi api(token.empty() ? account.server : server, credential);
    return repository(api, args, path_utf8(cwd), out, git_log);
}

}
