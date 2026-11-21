#include "desktop.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace loki {
namespace {

constexpr auto device = "LOKI Desktop";
#ifdef _WIN32
constexpr auto platform = "Windows";
#elif defined(__APPLE__)
constexpr auto platform = "macOS";
#else
constexpr auto platform = "Linux";
#endif

std::string scope_for(const std::string& server) {
    try {
        ManagementApi::origin(server);
    } catch (const std::runtime_error&) {
        return {};
    }
    try {
        ManagementApi(server).request("GET", "/api/v1");
    } catch (const ApiError& error) {
        if (error.status == 404) return {};
        throw;
    } catch (const nlohmann::json::parse_error&) {
        return {};
    }
    return "repositories";
}

}

SignIn Desktop::pair(const std::string& address) {
    const auto scope = scope_for(address);
    if (scope.empty()) {
        Api api(address);
        return {api.base(), scope, api.pair(device, platform)};
    }
    ManagementApi api(address);
    return {api.server(), scope, api.pair(scope, device, platform)};
}

std::optional<Session> Desktop::wait(const SignIn& request, const std::function<bool()>& active) {
    const auto& pairing = request.pairing;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(pairing.expires_in);
    while (active() && std::chrono::steady_clock::now() < deadline) {
        const auto next = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, pairing.interval));
        while (active() && std::chrono::steady_clock::now() < std::min(deadline, next))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!active() || std::chrono::steady_clock::now() >= deadline) break;
        auto session = request.scope.empty() ? Api(request.server).poll(pairing.device_code)
            : ManagementApi(request.server).poll(request.scope, pairing.device_code);
        if (session && active()) return session;
        if (session) {
            try { Api(request.server, session->token).sign_out(); } catch (const std::exception&) {}
        }
    }
    if (!active()) return std::nullopt;
    throw std::runtime_error("Pairing expired. Start pairing again.");
}

void Desktop::login(const std::string& server, const std::string& username,
                    const std::string& password, const std::string& code) {
    const auto scope = scope_for(server);
    const auto session = scope.empty() ? Api(server).login(username, password, code, device, platform)
        : ManagementApi(server).login(scope, username, password, code, device, platform);
    remember(server, session);
}

void Desktop::remember(const std::string& server, const Session& session) {
    const auto previous = store_.account();
    const auto& old = previous.session;
    try {
        store_.sign_in(server, session, old.token);
    } catch (...) {
        try { Api(server, session.token).sign_out(); } catch (const std::exception&) {}
        throw;
    }
    if (!old.token.empty() && old.token != session.token) {
        try { Api(previous.server, old.token).sign_out(); } catch (const std::exception&) {}
    }
}

void Desktop::logout(bool local) {
    const auto account = store_.account();
    if (!local && !account.session.token.empty()) {
        Api current(account.server, account.session.token);
        try { current.sign_out(); }
        catch (const ApiError& error) { if (error.status != 401) throw; }
        const auto legacy = FileStore(store_.directory()).get();
        if (!legacy.session.token.empty() && legacy.session.token != account.session.token) {
            try { FilesApi(legacy.server, legacy.session.token).sign_out(); } catch (const std::exception&) {}
        }
    }
    store_.sign_out(account.session.token);
}

void Desktop::watch(Api api, const std::function<bool()>& active, const std::function<void()>& changed, int wait) {
    std::string state;
    while (active()) {
        try {
            const bool first = state.empty();
            const auto result = api.watch(state, wait);
            state = result.state;
            if (result.changed && !first && active()) changed();
        } catch (const std::exception&) {
            for (int i = 0; i < 15 && active(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void Desktop::map(const std::string& folder, const fs::path& destination) {
    const auto path = fs::absolute(destination).lexically_normal();
    if (!fs::is_directory(path)) throw std::runtime_error("Choose an existing local folder.");
    store_.save_mapping({folder, path});
}

std::map<std::string, Outcome> Desktop::sync(const std::string& folder) {
    if (folder.empty()) return Engine(store_).sync_all();
    const auto mapping = store_.mapping(folder);
    if (!mapping) throw std::runtime_error("This folder has no local mapping.");
    return {{folder, Engine(store_).sync(*mapping)}};
}

void Desktop::download(FilesApi api, const fs::path& cache, const std::string& share, const std::string& path,
                       const fs::path& target, const std::function<bool(std::int64_t, std::int64_t)>& progress,
                       const std::function<bool()>& active, const FileMetadata* expected) {
    fs::create_directories(cache);
    ProfileLock transfer(cache / "download.lock");
    const auto temporary = api.download(share, path, cache, progress, expected);
    if (!active()) throw std::runtime_error("Download cancelled.");
    const auto destination = fs::absolute(target);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto staged = destination.parent_path() / (".loki-download-" + std::to_string(suffix));
    try {
        fs::copy_file(temporary, staged);
        if (!active()) throw std::runtime_error("Download cancelled.");
        fs::rename(staged, destination);
    } catch (...) {
        std::error_code ignored;
        fs::remove(staged, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(temporary, ignored);
}

}
