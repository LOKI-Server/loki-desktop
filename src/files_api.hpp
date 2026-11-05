#pragma once
#include "api.hpp"
#include <functional>

namespace loki {
struct Share { std::string id, name; };
struct ShareEntry {
    std::string name;
    bool directory = false;
    std::int64_t size = 0, modified = 0;
};
struct ShareListing {
    std::vector<ShareEntry> entries;
    std::int64_t total = 0;
    int page = 0;
};
struct FileMetadata {
    std::int64_t size = 0;
    std::string sha256, last_modified;
};

/** Read-only share access. Calls block and reuse the account device credential. */
class FilesApi {
public:
    FilesApi(std::string server, std::string token = {});
    void discover();
    Session login(const std::string& username, const std::string& password, const std::string& code,
                  const std::string& device, const std::string& platform);
    void sign_out();
    std::vector<Share> shares();
    ShareListing entries(const std::string& share, const std::string& path, int page = 0);
    FileMetadata metadata(const std::string& share, const std::string& path);
    /** Resume into a cache file; returns it only after size and SHA-256 verification.
     * Returning false from progress cancels, retaining the partial for retry. */
    std::filesystem::path download(const std::string& share, const std::string& path,
        const std::filesystem::path& cache,
        const std::function<bool(std::int64_t, std::int64_t)>& progress);
private:
    Api api_;
    std::string token_;
};

struct FileSession { std::string server; Session session; };
/** Legacy file-only credentials, kept for migration and cleanup only. */
class FileStore {
public:
    explicit FileStore(const std::filesystem::path& directory);
    const FileSession& get() const { return session_; }
    void save(const std::string& server, const Session& session);
    void clear();
private:
    std::filesystem::path file_;
    FileSession session_;
};
} // namespace loki
