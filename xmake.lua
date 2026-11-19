-- LOKI Client for desktop: keeps folders in step with a LOKI server's Save sync.
--
--   xmake                 # build (fetches wxWidgets, libcurl, nlohmann_json)
--   xmake run loki-sync   # start it
--   xmake test            # engine and hashing tests

set_project("loki-sync")
set_version("0.1.0")
set_xmakever("2.9.0")
set_languages("c++20")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")

option("gui")
    set_default(true)
    set_showmenu(true)
    set_description("Build the wxWidgets desktop frontend")
option_end()

add_requires("libcurl", "nlohmann_json")
if has_config("gui") then
    add_requires("wxwidgets")
end

-- Everything except the GUI, so tests can link it without wxWidgets.
target("loki-core")
    set_kind("static")
    add_files("src/api.cpp", "src/files_api.cpp", "src/engine.cpp", "src/index.cpp", "src/sha256.cpp", "src/store.cpp",
              "src/management_api.cpp", "src/process.cpp", "src/desktop.cpp", "src/commands.cpp", "src/apps_api.cpp")
    add_includedirs("src", {public = true})
    add_packages("libcurl", "nlohmann_json", {public = true})
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("crypt32", "advapi32", {public = true})
        add_cxflags("/utf-8", {public = true, tools = {"cl", "clang_cl"}})
    end

target("loki-git")
    set_kind("binary")
    set_basename("loki-git")
    add_deps("loki-core")
    add_files("src/cli.cpp")
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("crypt32")
        add_ldflags("-municode", {tools = {"gcc", "gxx"}})
    end

if has_config("gui") then
target("loki-sync")
    set_kind("binary")
    set_basename("loki-client")
    add_deps("loki-core")
    add_files("src/app.cpp", "src/main_frame.cpp", "src/files_dialog.cpp", "src/workspace_pages.cpp")
    add_packages("wxwidgets")
    if is_plat("windows") then
        add_rules("win.sdk.application")
        add_defines("UNICODE", "_UNICODE", "NOMINMAX")
        add_files("res/loki-sync.rc")
        -- The resource embeds wxWidgets' complete PerMonitorV2 manifest.
        add_ldflags("/MANIFEST:NO", {tools = "link", force = true})
        -- Explorer does not inherit the package PATH set by `xmake run`.
        -- Keep the prebuilt wxWidgets runtime beside the executable.
        after_build(function (target)
            for _, file in ipairs(target:pkg("wxwidgets"):libraryfiles()) do
                if path.extension(file):lower() == ".dll" then
                    os.cp(file, target:targetdir())
                end
            end
        end)
    end

end

target("loki-sync-tests")
    set_kind("binary")
    set_default(false)
    add_deps("loki-core")
    add_files("tests/*.cpp")
    add_tests("default")
