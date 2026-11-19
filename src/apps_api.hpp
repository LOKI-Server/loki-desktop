#pragma once

#include "desktop.hpp"

namespace loki {

struct AvailableApp {
    Share repository;
    PublishedApp app;
    std::optional<AppBuild> build;
    std::filesystem::path downloaded;
    bool update = false;
    std::string check_error;
};

struct AppCatalog {
    std::vector<AvailableApp> apps;
    std::vector<std::string> errors;
};

class AppDownloads {
public:
    explicit AppDownloads(Store& store);
    AppCatalog catalog();
    std::filesystem::path download(const AvailableApp& app,
        const std::function<bool(std::int64_t, std::int64_t)>& progress,
        const std::function<bool()>& active);
    static const char* platform();

private:
    std::filesystem::path location(const AvailableApp& app) const;
    StoredAccount account_;
    FilesApi files_;
    std::filesystem::path profile_;
};
}
