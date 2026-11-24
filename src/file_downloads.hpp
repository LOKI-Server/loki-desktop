#pragma once

#include "files_api.hpp"
#include "store.hpp"

namespace loki {

using DownloadProgress = std::function<bool(std::int64_t, std::int64_t)>;

struct TrackedDownload {
    std::string id, share, share_name, remote_path;
    std::filesystem::path destination;
    DownloadManifest baseline;
};

enum class DownloadStatus { current, available, missing, disabled, unavailable, check_failed };

struct DownloadCheck {
    TrackedDownload item;
    DownloadStatus status;
    bool can_download = false;
    std::string error;
};

class FileDownloads {
public:
    explicit FileDownloads(Store& store);

    void download(const Share& share, const std::string& path, bool directory,
                  const std::filesystem::path& destination, const DownloadProgress& progress);
    std::vector<DownloadCheck> check();
    void update(const std::string& id, const DownloadProgress& progress);
    void redownload(const std::string& id, const std::filesystem::path& destination, const DownloadProgress& progress);
    void locate(const std::string& id, const std::filesystem::path& destination);
    void forget(const std::string& id);

private:
    void replace(const TrackedDownload& item, const std::filesystem::path& destination,
                 bool repair, const DownloadProgress& progress);

    FilesApi api_;
    std::filesystem::path registry_, cache_;
};

} // namespace loki
