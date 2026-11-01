// Engine and hashing tests. The two-device test runs against a real LOKI server
// when LOKI_TEST_SERVER, LOKI_TEST_USER, and LOKI_TEST_PASSWORD are set.
#include <chrono>
#include <cstdlib>
#include <future>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

#include "engine.hpp"
#include "index.hpp"
#include "sha256.hpp"

namespace fs = std::filesystem;
using namespace loki;

static int failures = 0;
#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": check failed: " #condition "\n"; \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

static fs::path scratch(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("loki-sync-test-" + std::to_string(std::random_device{}()) + "-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}
static void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
static std::string read(const fs::path& file) { return read_text(file); }
/** Makes a file look old enough to be settled, as if written minutes ago. */
static void age(const fs::path& file) {
    fs::last_write_time(file, fs::last_write_time(file) - std::chrono::minutes(5));
}

static void hashing() {
    CHECK(Sha256::of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 million;
    const std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.update(block.data(), block.size());
    CHECK(million.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    auto dir = scratch("hash");
    write(dir / "f", "abc");
    CHECK(Sha256::of_file(dir / "f") == Sha256::of("abc"));
    fs::remove_all(dir);
}

static void indexes() {
    auto dir = scratch("index");
    SyncIndex index;
    index.cursor = 42;
    index.entries["saves/Pok\xC3\xA9mon.srm"] = {3, std::string(64, 'a'), 100, 1700000000000};
    index.entries["gone.sav"] = {5, std::nullopt, 0, 0};
    index.save(dir / "i.json");
    auto loaded = SyncIndex::load(dir / "i.json");
    CHECK(loaded.cursor == 42);
    CHECK(loaded.entries.size() == 2);
    CHECK(loaded.entries["saves/Pok\xC3\xA9mon.srm"].hash == std::string(64, 'a'));
    CHECK(!loaded.entries["gone.sav"].hash);
    CHECK(SyncIndex::load(dir / "missing.json").entries.empty());
    fs::remove_all(dir);
}

static void naming() {
    CHECK(Api::normalize(" loki.example.com/ ") == "https://loki.example.com");
    CHECK(Api::normalize("http://127.0.0.1:7070") == "http://127.0.0.1:7070");
    CHECK(ignored(".loki-tmp-123") && ignored("desktop.ini") && !ignored("Zelda.srm"));
    CHECK(Outcome{}.summary() == "Up to date");
    Outcome some;
    some.downloaded = 2;
    some.conflicts = 1;
    CHECK(some.summary() == "2 received \xC2\xB7 1 conflict kept on the server");
}

/** Two computers share a folder through a live server, including a conflict. */
static void two_devices(const std::string& server, const std::string& user, const std::string& password) {
    auto home_a = scratch("a"), home_b = scratch("b");
    Store a(home_a / "config"), b(home_b / "config");
    a.sign_in(server, Api(server).login(user, password, "", "Test A", "test"));
    b.sign_in(server, Api(server).login(user, password, "", "Test B", "test"));
    const auto folder = a.api().create_folder("desktop-test-" + std::to_string(std::random_device{}()), "generic");
    a.save_mapping({folder.id, home_a / "saves"});
    b.save_mapping({folder.id, home_b / "saves"});
    fs::create_directories(home_a / "saves");
    fs::create_directories(home_b / "saves");
    Engine sync_a(a), sync_b(b);

    write(home_a / "saves/Emerald.srm", "badge 1");
    write(home_a / "saves/gba/Fusion.srm", "fusion");
    age(home_a / "saves/Emerald.srm");
    age(home_a / "saves/gba/Fusion.srm");
    auto first = sync_a.sync(*a.mapping(folder.id));
    CHECK(first.uploaded == 2 && first.errors.empty());
    auto pulled = sync_b.sync(*b.mapping(folder.id));
    CHECK(pulled.downloaded == 2);
    CHECK(read(home_b / "saves/gba/Fusion.srm") == "fusion");

    // Both edit offline; B syncs second, so A's save is kept on the server as a conflict.
    write(home_a / "saves/Emerald.srm", "badge 2 on A");
    write(home_b / "saves/Emerald.srm", "badge 3 on B");
    age(home_a / "saves/Emerald.srm");
    age(home_b / "saves/Emerald.srm");
    CHECK(sync_a.sync(*a.mapping(folder.id)).uploaded == 1);
    auto clash = sync_b.sync(*b.mapping(folder.id));
    CHECK(clash.conflicts == 1 && clash.errors.empty());
    CHECK(sync_a.sync(*a.mapping(folder.id)).downloaded == 1);
    CHECK(read(home_a / "saves/Emerald.srm") == "badge 3 on B");

    // A file that B never synced does not replace A's newer copy: it is stashed.
    write(home_b / "saves/new.sav", "from B");
    age(home_b / "saves/new.sav");
    CHECK(sync_b.sync(*b.mapping(folder.id)).uploaded == 1);
    fs::remove(b.index_file(folder.id));
    write(home_b / "saves/new.sav", "stale copy");
    // Without an index every file is new to B; let them all count as settled.
    for (const auto& entry : fs::recursive_directory_iterator(home_b / "saves")) {
        if (entry.is_regular_file()) age(entry.path());
    }
    auto fresh = sync_b.sync(*b.mapping(folder.id));
    CHECK(read(home_b / "saves/new.sav") == "from B");
    CHECK(fresh.conflicts >= 1);

    // Deletions travel, and a quiet pass changes nothing.
    fs::remove(home_a / "saves/gba/Fusion.srm");
    CHECK(sync_a.sync(*a.mapping(folder.id)).uploaded == 1);
    CHECK(sync_b.sync(*b.mapping(folder.id)).removed == 1);
    CHECK(!fs::exists(home_b / "saves/gba/Fusion.srm"));
    CHECK(sync_b.sync(*b.mapping(folder.id)).summary() == "Up to date");

    // A held watch wakes as soon as the other computer commits.
    auto watcher = a.api();
    const auto quiet = watcher.watch("", 0);
    auto woke = std::async(std::launch::async, [&] { return watcher.watch(quiet.state, 30); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write(home_b / "saves/live.sav", "live");
    age(home_b / "saves/live.sav");
    const auto started = std::chrono::steady_clock::now();
    CHECK(sync_b.sync(*b.mapping(folder.id)).uploaded == 1);
    const auto watched = woke.get();
    CHECK(watched.changed && watched.state != quiet.state);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    CHECK(watcher.watch(watched.state, 1).changed == false);

    // An emulator saving just as a download would replace its file keeps its save;
    // the incoming version stays on the server as a conflict instead of being lost.
    write(home_a / "saves/Emerald.srm", "badge 4 on A");
    age(home_a / "saves/Emerald.srm");
    CHECK(sync_a.sync(*a.mapping(folder.id)).uploaded == 1);
    sync_b.before_replace = [&](const std::string& path) {
        if (path != "Emerald.srm") return;
        write(home_b / "saves/Emerald.srm", "badge 5 saved on B mid-sync");
        age(home_b / "saves/Emerald.srm");
    };
    const auto raced = sync_b.sync(*b.mapping(folder.id));
    sync_b.before_replace = nullptr;
    CHECK(raced.downloaded == 0 && raced.errors.empty());
    CHECK(read(home_b / "saves/Emerald.srm") == "badge 5 saved on B mid-sync");
    const auto resolved = sync_b.sync(*b.mapping(folder.id));
    CHECK(resolved.uploaded == 1 && resolved.conflicts == 1);
    CHECK(sync_a.sync(*a.mapping(folder.id)).downloaded == 1);
    CHECK(read(home_a / "saves/Emerald.srm") == "badge 5 saved on B mid-sync");

    // A save still being written when the other computer's deletion arrives is kept.
    fs::remove(home_a / "saves/new.sav");
    CHECK(sync_a.sync(*a.mapping(folder.id)).uploaded == 1);
    write(home_b / "saves/new.sav", "saved on B before the deletion arrived");
    const auto kept = sync_b.sync(*b.mapping(folder.id));
    CHECK(kept.removed == 0);
    CHECK(read(home_b / "saves/new.sav") == "saved on B before the deletion arrived");

    // Files still being written wait for the next pass.
    write(home_a / "saves/busy.sav", "writing");
    CHECK(sync_a.sync(*a.mapping(folder.id)).uploaded == 0);

    a.api().sign_out();
    b.api().sign_out();
    fs::remove_all(home_a);
    fs::remove_all(home_b);
}

int main() try {
    hashing();
    indexes();
    naming();
    const char* server = std::getenv("LOKI_TEST_SERVER");
    const char* user = std::getenv("LOKI_TEST_USER");
    const char* password = std::getenv("LOKI_TEST_PASSWORD");
    if (server && user && password) {
        try {
            two_devices(server, user, password);
        } catch (const std::exception& e) {
            std::cerr << "two-device test failed: " << e.what() << "\n";
            ++failures;
        }
    } else {
        std::cout << "Skipping the two-device test; set LOKI_TEST_SERVER, LOKI_TEST_USER, and LOKI_TEST_PASSWORD.\n";
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All checks passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "unexpected error: " << e.what() << "\n";
    return 1;
}
