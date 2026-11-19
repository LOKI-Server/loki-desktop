#include "commands.hpp"
#include "process.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr auto usage = R"(LOKI command line

  loki-git auth login SERVER
  loki-git auth login SERVER --password-stdin USER [--mfa]
  loki-git auth status | logout [--local]
  loki-git repo list | view REPO
  loki-git repo create NAME [--push] [--branch NAME] [--remote NAME]
  loki-git repo push REPO [--branch NAME] | clone REPO [DIRECTORY]
  loki-git repo fetch REPO | pull REPO

loki-git uses its own profile. Set LOKI_GIT_CONFIG_DIR to choose its directory.
With --password-stdin, enter the password on the first line and the MFA code on
the second when --mfa is supplied. REPO accepts an ID or unambiguous name.
)";

volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

int auth(loki::Desktop& client, const std::vector<std::string>& args) {
    auto& store = client.store();
    if (args.size() < 2) throw std::runtime_error("Choose auth login, status, or logout.");
    const auto& action = args[1];
    if (action == "login") {
        if (args.size() < 3) throw std::runtime_error("Enter a server address.");
        bool mfa = false;
        std::string username;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--mfa") mfa = true;
            else if (args[i] == "--password-stdin" && i + 1 < args.size()) username = args[++i];
            else throw std::runtime_error("Unknown login option. See loki-git --help.");
        }
        if (!username.empty()) {
            std::string password, code;
            if (!std::getline(std::cin, password) || password.empty() || (mfa && !std::getline(std::cin, code)))
                throw std::runtime_error("Supply the password and any requested MFA code on standard input.");
            client.login(args[2], username, password, code);
        } else {
            if (mfa) throw std::runtime_error("--mfa is only used with --password-stdin.");
            const auto request = client.pair(args[2]);
            std::cout << "Open " << request.pairing.verification_uri << "\nApprove code " << request.pairing.user_code
                      << " for " << (request.scope.empty() ? "account" : request.scope) << " access.\n" << std::flush;
            if (const auto session = client.wait(request, [] { return !interrupted; })) client.remember(request.server, *session);
            else return 130;
        }
        std::cout << "Signed in as " << store.session().username << ".\n";
    } else if (action == "status" && args.size() == 2) {
        std::cout << client.status().dump(2) << '\n';
    } else if (action == "logout" && (args.size() == 2 || (args.size() == 3 && args[2] == "--local"))) {
        client.logout(args.size() == 3);
        std::cout << "Signed out.\n";
    } else {
        throw std::runtime_error("Choose auth login, status, or logout. See loki-git --help.");
    }
    return 0;
}

int execute(std::vector<std::string> args) {
    if (args.empty() || args.front() == "--help" || args.front() == "-h") {
        std::cout << usage;
        return 0;
    }
    loki::Store store(loki::user_directory(loki::Profile::Git), loki::Profile::Git);
    loki::Desktop client(store);
    if (args[0] == "auth") return auth(client, args);
    if (args[0] != "repo") throw std::runtime_error("Choose auth or repo. See loki-git --help.");
    args.erase(args.begin());
    const auto* token = std::getenv("LOKI_TOKEN");
    const auto* server = std::getenv("LOKI_SERVER");
    if (token && *token && (!server || !*server)) throw std::runtime_error("LOKI_TOKEN requires LOKI_SERVER.");
    return loki::git_command(client, std::move(args), fs::current_path(), std::cout,
                                 server ? server : "", token ? token : "");
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    try {
        std::signal(SIGINT, interrupt);
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
            args.push_back(loki::narrow(argv[i]));
#else
            args.emplace_back(argv[i]);
#endif
        }
        return execute(std::move(args));
    } catch (const std::exception& error) {
        std::cerr << "loki-git: " << error.what() << '\n';
        return 1;
    }
}
