#include "apps_api.hpp"
#include "sha256.hpp"

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace loki {
namespace {

fs::path filename(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' '
        || name.find_first_of("/\\:<>\"|?*\r\n") != std::string::npos || name.find('\0') != std::string::npos) {
        throw std::runtime_error("The repository returned an invalid package filename.");
    }
    return path_from_utf8(name);
}

}

AppDownloads::AppDownloads(Store& store)
    : account_(store.account()), files_(account_.server, account_.session.token), profile_(store.directory()) {
    if (account_.session.token.empty()) throw std::runtime_error("Sign in to browse app repositories.");
}

const char* AppDownloads::platform() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

fs::path AppDownloads::location(const AvailableApp& app) const {
    const auto key = Sha256::of(account_.server + '\0' + app.repository.id + '\0' + app.app.id + '\0' + platform());
    return profile_ / "apps" / key;
}

AppCatalog AppDownloads::catalog() {
    AppCatalog result;
    for (const auto& repository : files_.app_repositories()) {
        try {
            for (auto& entry : files_.catalog(repository.id)) {
                AvailableApp app{repository, std::move(entry), {}, {}, false, {}};
                if (const auto found = app.app.builds.find(platform()); found != app.app.builds.end()) {
                    app.build = found->second;
                }
                const auto dir = location(app);
                const auto saved = json::parse(read_text(dir / "download.json"), nullptr, false);
                if (saved.is_object()) {
                    try {
                        const auto path = dir / "packages" / filename(saved.at("filename"));
                        if (fs::is_regular_file(path)) {
                            app.downloaded = path;
                            if (app.build) {
                                const auto current = files_.metadata(repository.id, app.build->path);
                                app.update = saved.value("sha256", "") != current.sha256 || saved.value("path", "") != app.build->path;
                            }
                        }
                    } catch (const std::exception& error) {
                        app.check_error = error.what();
                    }
                }
                result.apps.push_back(std::move(app));
            }
        } catch (const std::exception& error) {
            result.errors.push_back(repository.name + ": " + error.what());
        }
    }
    return result;
}

fs::path AppDownloads::download(const AvailableApp& app,
    const std::function<bool(std::int64_t, std::int64_t)>& progress, const std::function<bool()>& active) {
    if (!app.build) throw std::runtime_error("This app has no build for this operating system.");
    const auto directory = location(app);
    const auto destination = directory / "packages" / filename(app.build->filename);
    fs::create_directories(destination.parent_path());
    ProfileLock update(directory / "update.lock");
    const auto current = files_.metadata(app.repository.id, app.build->path);
    Desktop::download(files_, profile_ / "downloads", app.repository.id, app.build->path,
                      destination, progress, active, &current);
    write_atomically(directory / "download.json", json{{"filename", app.build->filename},
        {"sha256", current.sha256}, {"path", app.build->path}}.dump(2));
    return destination;
}

}
