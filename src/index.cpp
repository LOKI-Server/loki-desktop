#include "index.hpp"

#include <nlohmann/json.hpp>

#include "store.hpp"

using nlohmann::json;

namespace loki {

void SyncIndex::save(const std::filesystem::path& file) const {
    json files = json::object();
    for (const auto& [path, e] : entries) {
        files[path] = {{"v", e.version}, {"h", e.hash ? json(*e.hash) : json(nullptr)}, {"s", e.size}, {"m", e.mtime}};
    }
    write_atomically(file, json{{"cursor", cursor}, {"files", files}}.dump());
}

SyncIndex SyncIndex::load(const std::filesystem::path& file) {
    SyncIndex index;
    if (!std::filesystem::exists(file)) return index;
    auto o = json::parse(read_text(file), nullptr, false);
    if (!o.is_object()) return index;
    index.cursor = o.value("cursor", std::int64_t(0));
    const auto files = o.value("files", json::object());
    for (const auto& [path, e] : files.items()) {
        Entry entry{e.value("v", std::int64_t(0)), std::nullopt, e.value("s", std::int64_t(0)),
                    e.value("m", std::int64_t(0))};
        if (e.contains("h") && e["h"].is_string()) entry.hash = e["h"].get<std::string>();
        index.entries[path] = entry;
    }
    return index;
}

}  // namespace loki
