#pragma once

#include "files_api.hpp"
#include "store.hpp"

namespace loki {

/** Bytes received and expected; returning false cancels. A negative count only asks whether to go on. */
using DownloadProgress = std::function<bool(std::int64_t, std::int64_t)>;

/** Downloads shared files and whole folders, verifying every file's SHA-256. */
class FileDownloads {
public:
    explicit FileDownloads(Store& store);

    /**
     * Saves the file, or the folder with everything inside it, at `destination`.
     * A folder is assembled next to the destination and moved into place only
     * when complete, so a cancelled or failed download leaves nothing behind.
     */
    void download(const Share& share, const std::string& path, bool directory,
                  const std::filesystem::path& destination, const DownloadProgress& progress);

private:
    FilesApi api_;
    std::filesystem::path cache_;
};

} // namespace loki
