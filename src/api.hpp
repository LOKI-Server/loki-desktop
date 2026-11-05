#pragma once
// The LOKI /sync/v1 client. Calls block; run them off the UI thread.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace loki {

/** An error response from LOKI, with its stable machine-readable code when it has one. */
class ApiError : public std::runtime_error {
public:
    ApiError(long status, std::string code, const std::string& message)
        : std::runtime_error(message), status(status), code(std::move(code)) {}
    long status;
    std::string code;
};

struct Discovery {
    std::string name;
    int protocol = 0;
    std::int64_t max_file_bytes = 0;
};
struct Pairing {
    std::string device_code, user_code, verification_uri, verification_uri_complete;
    int interval = 5, expires_in = 600;
};
struct Session {
    std::string token, device_id, username, display_name;
    bool can_sync = true;
};
struct Folder {
    std::string id, name, kind;
    int keep_versions = 10;
    std::int64_t seq = 0;
};
struct RemoteChange {
    std::string path;
    std::int64_t version = 0;
    std::optional<std::string> hash;  // empty: deleted
    std::int64_t size = 0, mtime = 0, seq = 0;
};
struct Feed {
    std::int64_t seq = 0;
    bool more = false, reset = false;
    std::vector<RemoteChange> changes;
};
struct Quota {
    std::int64_t used = 0, limit = 0, max_file_bytes = 0;
};
/** Every folder's sequence number, and a token that changes whenever any does. */
struct Watch {
    std::string state;
    bool changed = false;
    std::vector<std::pair<std::string, std::int64_t>> folders;
};

/** One change sent in a commit. An empty hash deletes the path. */
struct LocalChange {
    std::string path;
    std::int64_t base_version = 0;
    std::optional<std::string> hash;
    std::int64_t size = 0, mtime = 0;
};
struct CommitResult {
    enum class Status { Ok, Conflict, Stashed, Rejected } status = Status::Conflict;
    std::string path;
    std::int64_t version = 0;
    std::optional<RemoteChange> current;
    std::string code, error;
};

class Api {
public:
    explicit Api(std::string base_url, std::string token = {});
    const std::string& base() const { return base_; }

    Discovery discover();
    Pairing pair(const std::string& device_name, const std::string& platform);
    /** Empty while the code is still waiting for approval. */
    std::optional<Session> poll(const std::string& device_code);
    Session login(const std::string& username, const std::string& password, const std::string& code,
                  const std::string& device_name, const std::string& platform);
    Quota quota();
    bool sync_access();
    void sign_out();

    std::vector<Folder> folders();
    Folder create_folder(const std::string& name, const std::string& kind);
    Feed changes(const std::string& folder, std::int64_t since, int limit = 1000);
    /** Waits up to `wait` seconds for any folder to differ from `state` (empty: answer at once). */
    Watch watch(const std::string& state, int wait);

    bool has_blob(const std::string& hash);
    void upload_blob(const std::string& hash, const std::filesystem::path& file);
    /** Streams a blob into `file`, replacing it. */
    void download_blob(const std::string& hash, const std::filesystem::path& file);
    std::vector<CommitResult> commit(const std::string& folder, const std::vector<LocalChange>& changes,
                                     const std::string& on_conflict);

    /** Accepts `loki.example.com`, `https://loki.example.com/`, and similar. */
    static std::string normalize(std::string url);

private:
    friend class FilesApi;
    struct Response {
        long status = 0;
        std::string body;
    };
    Response request(const std::string& method, const std::string& path, const std::string* json = nullptr,
                     const std::filesystem::path* upload = nullptr, const std::filesystem::path* download = nullptr);
    std::string ok(const std::string& method, const std::string& path, const std::string* json = nullptr);
    [[noreturn]] static void fail(const Response& r);

    std::string base_;
    std::string token_;
};

}  // namespace loki
