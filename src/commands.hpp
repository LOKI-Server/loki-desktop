#pragma once
#include "desktop.hpp"
#include <ostream>

namespace loki {
int git_command(Desktop& client, std::vector<std::string> words,
                    const std::filesystem::path& directory, std::ostream& output,
                    const std::string& server = {}, const std::string& token = {}, std::ostream* git_log = nullptr);
struct GitRepository { std::string id, name, role; };
std::vector<GitRepository> git_repositories(Store& store);
std::filesystem::path git_folder(Store& store, const std::string& id);
void link_git_folder(Store& store, const std::string& id, const std::filesystem::path& folder);
std::string git_status(const std::filesystem::path& folder);
void commit_git_changes(const std::filesystem::path& folder, const std::string& summary);
bool starts_at_sign_in();
void set_start_at_sign_in(bool enabled);
}
