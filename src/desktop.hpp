#pragma once
#include "engine.hpp"
#include "files_api.hpp"
#include "management_api.hpp"

namespace loki {

struct SignIn {
    std::string server, scope;
    Pairing pairing;
};

class Desktop {
public:
    explicit Desktop(Store& store) : store_(store) {}
    SignIn pair(const std::string& server);
    std::optional<Session> wait(const SignIn& pairing, const std::function<bool()>& active);
    void login(const std::string& server, const std::string& username,
               const std::string& password, const std::string& code);
    void remember(const std::string& server, const Session& session);
    void logout(bool local = false);
    nlohmann::json status();
    static void watch(Api api, const std::function<bool()>& active,
                      const std::function<void()>& changed, int wait = 60);
    void map(const std::string& folder, const std::filesystem::path& destination);
    void unmap(const std::string& folder) { store_.remove_mapping(folder); }
    std::map<std::string, Outcome> sync(const std::string& folder = {});
    static void download(FilesApi api, const std::filesystem::path& cache, const std::string& share,
                         const std::string& path, const std::filesystem::path& destination,
                         const std::function<bool(std::int64_t, std::int64_t)>& progress,
                         const std::function<bool()>& active, const FileMetadata* expected = nullptr);
    Store& store() { return store_; }
private:
    Store& store_;
};

}
