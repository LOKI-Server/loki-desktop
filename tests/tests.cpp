// Engine and hashing tests. The two-device test runs against a real LOKI server
// when LOKI_TEST_SERVER, LOKI_TEST_USER, and LOKI_TEST_PASSWORD are set.
#include <algorithm>
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
#include "files_api.hpp"
#include "file_downloads.hpp"
#include "commands.hpp"
#include "client_update.hpp"
#include <nlohmann/json.hpp>
#ifdef LOKI_HAS_SKALD
#include <skald_vcs.h>
#endif

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

static void deletion_guard() {
    CHECK(!suspicious_deletion(0, 400));
    CHECK(!suspicious_deletion(1, 1));
    CHECK(!suspicious_deletion(1, 3));
    CHECK(!suspicious_deletion(4, 6));
    CHECK(!suspicious_deletion(10, 100));
    CHECK(suspicious_deletion(2, 2));
    CHECK(suspicious_deletion(312, 312));
    CHECK(suspicious_deletion(5, 8));
    CHECK(suspicious_deletion(50, 10000));
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

static void file_sessions() {
    auto dir = scratch("file-sessions");
    Store sync(dir);
    sync.sign_in("https://sync.example", {"sync-token","sync-device","reader","Reader"});
    sync.save_mapping({"save-folder", dir / "saves"});
    FileStore files(dir);
    CHECK(files.get().session.token.empty());
    files.save("https://files.example/", {"files-token","files-device","reader","Reader"});
    FileStore loaded(dir);
    CHECK(loaded.get().server == "https://files.example");
    CHECK(loaded.get().session.token == "files-token");
    loaded.clear();
    CHECK(FileStore(dir).get().session.token.empty());
    CHECK(sync.token() == "sync-token");
    CHECK(sync.mapping("save-folder").has_value());
    // An existing primary login always wins over a separate legacy login.
    files.save("https://legacy.example", {"legacy-token","old-device","other","Other"});
    CHECK(Store(dir).token() == "sync-token");
    sync.sign_out();
    CHECK(!Store(dir).signed_in()); // Sign-out cannot resurrect legacy credentials.
    auto migrated_dir = dir / "migration";
    FileStore(migrated_dir).save("https://files.example", {"file-token","file-device","reader","Reader"});
    Store migrated(migrated_dir);
    CHECK(migrated.token() == "file-token");
    CHECK(migrated.server() == "https://files.example");
    CHECK(FileStore(migrated_dir).get().session.token.empty());
    fs::remove_all(dir);
}

static void shared_profile() {
    const auto dir = scratch("shared-profile");
    Store gui(dir), other(dir);
    gui.sign_in("https://loki.example", {"old-token","old-id","alice","Alice",true,"repositories"});
    CHECK(other.session().scope == "repositories");
    auto first = std::async(std::launch::async, [&] { gui.save_mapping({"first", dir / "a"}); });
    auto second = std::async(std::launch::async, [&] { other.save_mapping({"second", dir / "b"}); });
    first.get();
    second.get();
    CHECK(gui.mappings().size() == 2);
    CHECK(other.mappings().size() == 2);
    other.sign_in("https://loki.example", {"new-token","new-id","alice","Alice",true,"owner"}, "old-token");
    CHECK(gui.token() == "new-token");
    CHECK(gui.session().scope == "owner");
    try { gui.sign_out("old-token"); CHECK(false); }
    catch (const std::runtime_error&) {}
    CHECK(other.mappings().size() == 2);
    const auto legacy = dir / "legacy";
    write(legacy / "config.json", R"({"profile":"git"})");
    try { Store wrong(legacy); CHECK(false); }
    catch (const std::runtime_error&) {}
    CHECK(gui.signed_in());
    other.sign_out("new-token");
    CHECK(!gui.signed_in() && gui.mappings().empty());
    fs::remove_all(dir);
}

/** Exercises the Files API against the local HTTP test fixture. */
static void shared_files(const std::string& server) {
    FilesApi public_api(server);
    public_api.discover();
    try { FilesApi(server+"/legacy").discover(); CHECK(false); }
    catch (const std::runtime_error& e) { CHECK(std::string(e.what()).find("Update LOKI Server") != std::string::npos); }
    const auto session = public_api.login("reader","test-password","","Desktop test","test");
    CHECK(session.token == "download-token");
    FilesApi api(server,session.token);
    CHECK(api.shares().at(0).name == "Archive");
    const auto listing = api.entries("archive","Games/日本語 + #",1);
    CHECK(listing.page == 1 && listing.total == 101);
    CHECK(listing.entries.at(0).name == "日本語 + #.bin");
    auto dir = scratch("shared-files");
    Store account(dir / "account");
    account.sign_in(server,session);
    CHECK(account.files_api().shares().at(0).name == "Archive");
    CHECK(account.api().sync_access()); // The same saved credential serves both APIs.
    FileDownloads downloads(account);
    const auto share = api.shares().at(0);
    const auto active = [](auto, auto) { return true; };
    const auto game = dir / "Game";
    downloads.download(share, "Game", true, game, active);
    CHECK(fs::is_directory(game / "assets" / "empty"));
    CHECK(read(game / "assets" / "keep.txt") == "unchanged");
    CHECK(read(game / "old.txt") == "remove");
    const auto executable = game / "game.exe";
    const auto before = Sha256::of_file(executable);
    auto tracked = FileDownloads(account).check().at(0);
    CHECK(tracked.status == DownloadStatus::current);
    write(game / "user-save.dat", "my game save");
    const auto id = tracked.item.id;
    api.sign_out(); // The fixture publishes a changed game at the same source path.
    CHECK(downloads.check().at(0).status == DownloadStatus::available);
    try { downloads.update(id, [](auto, auto) { return false; }); CHECK(false); }
    catch (const std::runtime_error&) { CHECK(Sha256::of_file(executable) == before); }
    write(executable, "local customization");
    try { downloads.update(id, active); CHECK(false); }
    catch (const std::runtime_error&) { CHECK(read(executable) == "local customization"); }
    fs::remove(executable);
    // Restore the original baseline so this update exercises binary patching.
    const auto baseline = api.download("archive", "baseline", dir, active);
    fs::copy_file(baseline, executable);
    CHECK(Sha256::of_file(executable) == before);
    try { downloads.update(id, active); CHECK(false); }
    catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find("changed during the update") != std::string::npos);
        CHECK(Sha256::of_file(executable) == before && fs::exists(game / "old.txt"));
    }
    downloads.update(id, active);
    CHECK(Sha256::of_file(executable) == api.metadata("archive", "Game/game.exe").sha256);
    CHECK(!fs::exists(game / "old.txt"));
    CHECK(read(game / "new.txt") == "added");
    CHECK(read(game / "user-save.dat") == "my game save");
    CHECK(downloads.check().at(0).status == DownloadStatus::current);

    const auto moved = dir / "Moved";
    fs::rename(game, moved);
    CHECK(downloads.check().at(0).status == DownloadStatus::missing);
    write(moved / "assets" / "keep.txt", "my local changes");
    downloads.locate(id, moved);
    CHECK(downloads.check().at(0).item.destination == moved);
    downloads.download(share, "Game/new.txt", false, game, active);
    auto checks = downloads.check();
    CHECK(checks.size() == 2);
    for (const auto& check : checks) if (check.item.id != id) downloads.forget(check.item.id);
    fs::remove_all(moved);
    CHECK(downloads.check().at(0).can_download);
    downloads.redownload(id, moved, active);
    CHECK(read(moved / "new.txt") == "added");
    downloads.forget(id);
    CHECK(downloads.check().empty() && fs::exists(moved / "game.exe"));

    const auto solo = dir / "solo.txt";
    downloads.download(share, "Game/new.txt", false, solo, active);
    CHECK(downloads.check().at(0).item.destination == solo);
    CHECK(read(solo) == "added");
    downloads.forget(downloads.check().at(0).item.id);

    const auto original = api.download("archive","日本語 + #.bin",dir,[](auto,auto) { return true; });
    const auto contents = read(original);
    CHECK(!contents.empty());
    const auto meta = api.metadata("archive","file");
    CHECK(meta.sha256 == Sha256::of(contents));
    auto partial = [&](const std::string& path) {
        const auto metadata = api.metadata("archive",path);
        const auto key = Sha256::of(Api::normalize(server) + '\0' + "archive" + '\0' + path + '\0' + metadata.sha256);
        const auto file = dir/(key+".part");
        write(file,contents.substr(0,65536));
        return file;
    };
    for (const auto path : {"file","restart"}) {
        partial(path);
        const auto received=api.download("archive",path,dir,[](auto,auto) { return true; });
        CHECK(read(received)==contents);
    }
    const auto bad_range=partial("badRange");
    try { api.download("archive","badRange",dir,[](auto,auto) { return true; }); CHECK(false); }
    catch (const std::runtime_error&) { CHECK(fs::file_size(bad_range)==65536); }
    try { api.download("archive","interrupt",dir,[](auto,auto) { return true; }); CHECK(false); }
    catch (const std::runtime_error&) {
        const auto key=Sha256::of(Api::normalize(server)+'\0'+"archive"+'\0'+"interrupt"+'\0'+meta.sha256);
        CHECK(fs::file_size(dir/(key+".part"))==65536);
    }
    try { api.download("archive","badHash",dir,[](auto,auto) { return true; }); CHECK(false); }
    catch (const std::runtime_error&) {
        const auto key=Sha256::of(Api::normalize(server)+'\0'+"archive"+'\0'+"badHash"+'\0'+std::string(64,'0'));
        CHECK(!fs::exists(dir/(key+".part")));
    }
    CHECK(fs::file_size(api.download("archive","empty",dir,[](auto,auto) { return true; }))==0);
    try { api.download("archive","cancel",dir,[](auto received,auto) { return received==0; }); CHECK(false); }
    catch (const std::runtime_error&) {}
    CHECK(read(api.download("archive","cancel",dir,[](auto,auto) { return true; }))==contents);
    try { FilesApi(server,"revoked").shares(); CHECK(false); }
    catch (const ApiError& e) { CHECK(e.status==401 && e.code=="unauthorized"); }
    try { api.entries("archive","forbidden"); CHECK(false); }
    catch (const ApiError& e) { CHECK(e.status==403 && e.code=="forbidden"); }
    api.sign_out();
    fs::remove_all(dir);
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
    age(home_a / "saves/busy.sav");
    CHECK(sync_a.sync(*a.mapping(folder.id)).uploaded == 1);
    CHECK(sync_b.sync(*b.mapping(folder.id)).errors.empty());

    // A folder that vanishes, as when its drive is unplugged, changes nothing.
    fs::rename(home_a / "saves", home_a / "unplugged");
    const auto unplugged = sync_a.sync(*a.mapping(folder.id));
    CHECK(unplugged.uploaded == 0 && !unplugged.errors.empty());
    // An empty folder in its place holds the deletions until the person decides.
    fs::create_directories(home_a / "saves");
    const auto emptied = sync_a.sync(*a.mapping(folder.id));
    CHECK(emptied.uploaded == 0 && emptied.held >= 2);
    CHECK(a.mapping(folder.id)->held == emptied.held && a.mapping(folder.id)->failed);
    CHECK(sync_b.sync(*b.mapping(folder.id)).removed == 0);
    // Bringing them back downloads every missing file again.
    a.decide(folder.id, restore_missing);
    const auto restored = sync_a.sync(*a.mapping(folder.id));
    CHECK(restored.downloaded == emptied.held && restored.held == 0);
    CHECK(read(home_a / "saves/Emerald.srm") == "badge 5 saved on B mid-sync");
    CHECK(a.mapping(folder.id)->decision.empty() && a.mapping(folder.id)->held == 0);
    // Deleting them everywhere is a deliberate choice that goes through.
    for (const auto& entry : fs::directory_iterator(home_a / "saves")) fs::remove_all(entry.path());
    CHECK(sync_a.sync(*a.mapping(folder.id)).held >= 2);
    a.decide(folder.id, delete_missing);
    const auto confirmed = sync_a.sync(*a.mapping(folder.id));
    CHECK(confirmed.held == 0 && confirmed.uploaded >= 2);
    CHECK(sync_b.sync(*b.mapping(folder.id)).removed >= 2);

    a.api().sign_out();
    b.api().sign_out();
    fs::remove_all(home_a);
    fs::remove_all(home_b);
}

#ifdef LOKI_HAS_SKALD
static void skald_working_copy() {
    const auto directory = scratch("skald");
    const auto folder = directory / "worktree";
    SkaldRepo* repo = nullptr;
    if (skald_init(path_utf8(folder).c_str(), &repo) != SKALD_OK)
        throw std::runtime_error(skald_last_error());
    skald_repo_free(repo);

    const auto nested = folder / "assets";
    write(nested / "level.txt", "map data");
    CHECK(repository_status(nested).changes.find("assets/level.txt") != std::string::npos);
    commit_repository_changes(nested, "Add level", "Desktop Tester");
    auto status = repository_status(nested);
    CHECK(status.changes.find("Working copy is clean.") != std::string::npos);
    CHECK(status.current_branch.has_value());
    const auto original = status.current_branch.value();

    create_repository_branch(nested, "feature/level");
    status = repository_status(nested);
    CHECK(status.branches.size() == 2);
    CHECK(status.current_branch == original);
    switch_repository_branch(nested, "feature/level");
    CHECK(repository_status(nested).current_branch == "feature/level");
    create_repository_branch(nested, "temporary");
    delete_repository_branch(nested, "temporary");
    CHECK(repository_status(nested).branches.size() == 2);

    write(nested / "level.txt", "new map data");
    commit_repository_changes(nested, "Update level", "Desktop Tester");
    write(nested / "level.txt", "uncommitted map data");
    try { switch_repository_branch(nested, original); CHECK(false); }
    catch (const std::runtime_error&) {}
    CHECK(repository_status(nested).current_branch == "feature/level");
    CHECK(read(nested / "level.txt") == "uncommitted map data");
    write(nested / "level.txt", "new map data");
    switch_repository_branch(nested, original);
    CHECK(read(nested / "level.txt") == "map data");
    try { delete_repository_branch(nested, "feature/level"); CHECK(false); }
    catch (const std::runtime_error&) {}
    CHECK(repository_status(nested).branches.size() == 2);

    Store store(directory / "profile");
    const auto* id = "01234567-89ab-cdef-0123-456789abcdef";
    link_repository_folder(store, id, nested);
    CHECK(fs::equivalent(repository_folder(store, id), folder));
    fs::remove_all(directory);
}

static void link_plain_repository_folder() {
    const auto directory = scratch("skald-link");
    const auto folder = directory / "project";
    const auto nested = folder / "assets";
    write(nested / "level.txt", "map data");
    write(folder / ".gitignore", "build/\n");
    const auto plain = inspect_repository_folder(folder);
    CHECK(fs::equivalent(plain.root, folder));
    CHECK(!plain.initialized && plain.can_rename_ignore);
    CHECK(!fs::exists(folder / ".skald"));

    initialize_repository_folder(plain.root);
    initialize_repository_folder(plain.root);
    const auto initialized = inspect_repository_folder(nested);
    CHECK(initialized.initialized && initialized.can_rename_ignore);
    CHECK(fs::equivalent(initialized.root, folder));
    Store store(directory / "profile");
    const auto* id = "01234567-89ab-cdef-0123-456789abcdef";
    link_repository_folder(store, id, initialized.root);
    CHECK(fs::equivalent(repository_folder(store, id), folder));
    CHECK(read(nested / "level.txt") == "map data");

    rename_repository_ignore(initialized.root);
    CHECK(!fs::exists(folder / ".gitignore"));
    CHECK(read(folder / ".skaldignore") == "build/\n");
    write(folder / "build/ignored.txt", "ignore me");
    CHECK(repository_status(folder).changes.find("ignored.txt") == std::string::npos);

    write(folder / ".gitignore", "cache/\n");
    CHECK(!inspect_repository_folder(nested).can_rename_ignore);
    try { rename_repository_ignore(folder); CHECK(false); }
    catch (const std::runtime_error&) {}
    CHECK(read(folder / ".gitignore") == "cache/\n");
    CHECK(read(folder / ".skaldignore") == "build/\n");
    fs::remove_all(directory);
}

static void skald_repository_workflows() {
    const auto folder = scratch("skald-workflows");
    constexpr auto remote_url = "https://loki.example/skald/v1/repos/project";
    SkaldRepo* repo = nullptr;
    if (skald_init(path_utf8(folder).c_str(), &repo) != SKALD_OK) throw std::runtime_error(skald_last_error());
    const auto remote_result = skald_remote(repo, "origin", remote_url);
    skald_repo_free(repo);
    CHECK(remote_result == SKALD_OK);
    CHECK(repository_status(folder, remote_url).sync == BranchSync::Unknown);

    write(folder / "notes/level.txt", "original level\n");
    write(folder / "title.txt", "original title\n");
    commit_repository_changes(folder, "Start project", "Desktop Tester");
    const auto initial = repository_status(folder);
    const auto initial_history = repository_history(folder);
    CHECK(initial_history.size() == 1);
    CHECK(initial_history.front().id == initial.revision);
    const auto initial_diff = repository_commit_diff(folder, initial_history.front());
    CHECK(initial_diff.size() == 2);
    CHECK(initial_diff.front().path == "notes/level.txt");
    CHECK(initial_diff.front().change == "added");
    const auto unborn_comparison = compare_repository_branch(folder, std::nullopt, initial.revision.value());
    CHECK(unborn_comparison.files.size() == 2 && unborn_comparison.incoming.size() == 1);

    create_repository_branch(folder, "artwork");
    switch_repository_branch(folder, "artwork");
    write(folder / "notes/level.txt", "remote level\n");
    write(folder / "title.txt", "unfinished title\n");
    stage_repository_paths(folder, {"title.txt"}, true);
    stage_repository_paths(folder, {"title.txt"}, false);
    stage_repository_paths(folder, {"notes/level.txt"}, true);
    const auto staged = repository_working_diff(folder, true);
    const auto unstaged = repository_working_diff(folder, false);
    CHECK(staged.size() == 1 && staged.front().path == "notes/level.txt");
    CHECK(staged.front().patch.value_or("").find("+remote level") != std::string::npos);
    CHECK(unstaged.size() == 1 && unstaged.front().path == "title.txt");
    commit_staged_repository_changes(folder, "Update level art", "Desktop Tester");
    const auto feature = repository_status(folder);
    CHECK(feature.staged.empty() && feature.unstaged == std::vector<std::string>{"title.txt"});
    const auto feature_files = repository_commit_diff(folder, repository_history(folder).front());
    CHECK(feature_files.size() == 1 && feature_files.front().path == "notes/level.txt");

    // Populate the persisted refs in the same format as an SDK fetch.
    const auto state_file = folder / ".skald" / "state.json";
    auto state = nlohmann::json::parse(read(state_file));
    state["fetched"]["origin"] = {
        {"branches/" + initial.current_branch.value(), feature.revision.value()},
        {"branches/team/new-level", feature.revision.value()},
        {"tags/v1", initial.revision.value()}
    };
    write(state_file, state.dump());
    CHECK(repository_status(folder, remote_url).sync == BranchSync::LocalOnly);
    switch_repository_branch(folder, initial.current_branch.value());
    const auto behind = repository_status(folder, remote_url);
    CHECK(behind.sync == BranchSync::Behind);
    CHECK(behind.remote_branches.size() == 2);
    CHECK(behind.remote_branches.back().name == "team/new-level");
    create_repository_branch(folder, "team/new-level", behind.remote_branches.back().revision.value());
    switch_repository_branch(folder, "team/new-level");
    CHECK(repository_status(folder, remote_url).sync == BranchSync::UpToDate);
    write(folder / "branch-note.txt", "additional update\n");
    stage_repository_paths(folder, {"branch-note.txt"}, true);
    commit_staged_repository_changes(folder, "Add branch note", "Desktop Tester");
    CHECK(repository_status(folder, remote_url).sync == BranchSync::Ahead);

    switch_repository_branch(folder, initial.current_branch.value());
    write(folder / "notes/level.txt", "local level\n");
    stage_repository_paths(folder, {"notes/level.txt"}, true);
    commit_staged_repository_changes(folder, "Change level locally", "Desktop Tester");
    const auto diverged = repository_status(folder, remote_url);
    CHECK(diverged.sync == BranchSync::Diverged);
    const auto target = feature.revision.value();
    const auto comparison = compare_repository_branch(folder, diverged.revision, target);
    CHECK(comparison.current_only.size() == 1 && comparison.current_only.front().message == "Change level locally");
    CHECK(comparison.incoming.size() == 1 && comparison.incoming.front().message == "Update level art");
    CHECK(comparison.files.size() == 1 && comparison.files.front().path == "notes/level.txt");

    const auto merge = [&] {
        return merge_repository_branch(folder, diverged.current_branch, diverged.revision, target, "Desktop Tester", "Merge artwork");
    };
    CHECK(merge() == std::vector<std::string>{"notes/level.txt"});
    auto pending = repository_status(folder);
    CHECK(pending.merging && pending.conflicts == std::vector<std::string>{"notes/level.txt"});
    abort_repository_merge(folder);
    CHECK(!repository_status(folder).merging);
    CHECK(merge() == std::vector<std::string>{"notes/level.txt"});
    write(folder / "notes/level.txt", "resolved level\n");
    stage_repository_paths(folder, {"notes/level.txt"}, true);
    pending = repository_status(folder);
    CHECK(pending.merging && pending.conflicts.empty());
    commit_staged_repository_changes(folder, "Resolve merge", "Desktop Tester");
    CHECK(repository_history(folder).front().parents.size() == 2);
    CHECK(repository_status(folder).unstaged == std::vector<std::string>{"title.txt"});

    const auto resolved = repository_status(folder);
    const auto remote_checkout = std::ranges::find(resolved.branches, "team/new-level", &RepositoryBranch::name);
    if (remote_checkout == resolved.branches.end()) throw std::runtime_error("The fetched branch was not checked out.");
    CHECK(merge_repository_branch(folder, resolved.current_branch, resolved.revision,
                                  remote_checkout->revision.value(), "Desktop Tester", "Merge branch note").empty());
    CHECK(repository_history(folder).front().message == "Merge branch note");
    CHECK(read(folder / "branch-note.txt") == "additional update\n");
    CHECK(read(folder / "notes/level.txt") == "resolved level\n");

    create_repository_branch(folder, "fast-forward", initial.revision.value());
    switch_repository_branch(folder, "fast-forward");
    CHECK(merge_repository_branch(folder, "fast-forward", initial.revision, target,
                                  "Desktop Tester", "Fast-forward artwork").empty());
    CHECK(repository_status(folder).revision == feature.revision);
    fs::remove_all(folder);
}
#endif

static void client_update_metadata() {
    auto release = nlohmann::json{
        {"tag_name", "v0.10.0"}, {"draft", false}, {"prerelease", false},
        {"assets", nlohmann::json::array({{
            {"name", "LOKI-Client-0.10.0-windows-x64.zip"},
            {"size", 512}, {"digest", "sha256:" + std::string(64, 'a')},
            {"browser_download_url", "https://github.com/LOKI-Server/loki-desktop/releases/download/v0.10.0/LOKI-Client-0.10.0-windows-x64.zip"}
        }})}
    };
    const auto update = select_client_update(release.dump(), "0.2.0");
    CHECK(update && update->version == "0.10.0" && update->size == 512);
    CHECK(!select_client_update(release.dump(), "0.10.1"));
    release["prerelease"] = true;
    CHECK(!select_client_update(release.dump(), "0.2.0"));
    release["prerelease"] = false;
    auto& asset = release["assets"][0];
    const auto url = asset["browser_download_url"];
    asset["browser_download_url"] = "https://example.com/wrong.zip";
    try { select_client_update(release.dump(), "0.2.0"); CHECK(false); }
    catch (const std::runtime_error&) {}
    asset["browser_download_url"] = url;
    asset["digest"] = nullptr;
    try { select_client_update(release.dump(), "0.2.0"); CHECK(false); }
    catch (const std::runtime_error&) {}
}
// Repository listings from LOKI 0.9 name owners and offer groups; older ones don't.
static void repository_listings() {
    const auto old = read_hosted_repositories(nlohmann::json::parse(R"({"repositories":[
        {"id":"a","name":"game","role":"manage","remote_url":"x"},{"id":"b","name":"hidden","role":"read"}],"create":true})"), "sam");
    CHECK(old.repositories.size() == 1);
    CHECK(old.repositories[0].label() == "game");
    CHECK(old.owners.size() == 1 && old.owners[0].group.empty() && old.owners[0].label == "You (@sam)");
    CHECK(old.transfers_waiting == 0);

    const auto now = read_hosted_repositories(nlohmann::json::parse(R"({"repositories":[
        {"id":"a","name":"game","role":"write","remote_url":"x","owner":{"kind":"group","id":"g1","name":"Kiwis"}},
        {"id":"b","name":"game","role":"manage","remote_url":"y","owner":{"kind":"user","id":"u","name":"sam"}}],
        "create":false,"groups":[{"id":"g1","name":"Kiwis","icon":true}],
        "transfers":[{"repo_id":"c","can_decide":true},{"repo_id":"d","can_decide":false}]})"), "sam");
    CHECK(now.repositories.size() == 2);
    CHECK(now.repositories[0].label() == "Kiwis / game");
    CHECK(now.repositories[1].label() == "sam / game");
    // Without permission to create personal repositories, only the group is offered.
    CHECK(now.owners.size() == 1 && now.owners[0].group == "g1" && now.owners[0].label == "Group: Kiwis");
    CHECK(now.transfers_waiting == 1);
}

int main() try {
    client_update_metadata();
    repository_listings();
#ifdef LOKI_HAS_SKALD
    skald_working_copy();
    link_plain_repository_folder();
    skald_repository_workflows();
#endif
    hashing();
    deletion_guard();
    indexes();
    naming();
    file_sessions();
    shared_profile();
    if (const auto* fixture = std::getenv("LOKI_FILES_TEST_SERVER")) shared_files(fixture);
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
