-- LOKI Client for desktop: keeps folders in step with a LOKI server's Save sync.
--
--   xmake                 # build (fetches wxWidgets, libcurl, nlohmann_json)
--   xmake run loki-sync   # start it
--   xmake test            # engine and hashing tests

set_project("loki-sync")
set_version("0.2.1")
set_xmakever("2.9.0")
set_languages("c++20")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")

option("gui")
    set_default(true)
    set_showmenu(true)
    set_description("Build the wxWidgets desktop frontend")
option_end()

-- The release contains an MSVC import library and its matching x64 DLL.
package("skald-vcs")
    set_kind("library")
    set_license("MIT")
    add_urls("https://github.com/LOKI-Server/skald-vcs/releases/download/v$(version)/skald-vcs-$(version)-x86_64-pc-windows-msvc.zip")
    add_versions("0.2.0", "c30c701f93769d7c761f03228699a34b4f979791954f3dfce91fa849121e8dc6")
    add_versions("0.1.0", "7cd220c5ee8b9f253a540202c41c2357bf18b4b83759d0856bb23a36e1a62de6")
    add_links("skald_vcs")
    on_install("windows", function (package)
        local source = "."
        if not os.isfile(path.join(source, "include", "skald_vcs.h")) then
            source = "skald-vcs-" .. package:version_str() .. "-x86_64-pc-windows-msvc"
        end
        for _, dir in ipairs({"include", "lib", "bin", "share"}) do
            os.mkdir(package:installdir(dir))
        end
        os.cp(path.join(source, "include", "skald_vcs.h"), package:installdir("include"))
        os.cp(path.join(source, "lib", "skald_vcs.dll.lib"), path.join(package:installdir("lib"), "skald_vcs.lib"))
        os.cp(path.join(source, "lib", "skald_vcs.dll"), package:installdir("bin"))
        os.cp(path.join(source, "LICENSE"), path.join(package:installdir("share"), "SKALD-LICENSE"))
    end)
package_end()

local skald_supported = is_plat("windows") and is_arch("x64", "x86_64")
if skald_supported then
    add_requires("skald-vcs 0.2.0", {system = false})
end

add_requires("libcurl", "nlohmann_json")
if has_config("gui") then
    add_requires("wxwidgets")
end

-- Everything except the GUI, so tests can link it without wxWidgets.
target("loki-core")
    set_kind("static")
    add_files("src/api.cpp", "src/files_api.cpp", "src/engine.cpp", "src/index.cpp", "src/sha256.cpp", "src/store.cpp",
              "src/management_api.cpp", "src/client_update.cpp", "src/desktop.cpp", "src/commands.cpp", "src/apps_api.cpp")
    add_includedirs("src", {public = true})
    add_packages("libcurl", "nlohmann_json", {public = true})
    if skald_supported then
        add_packages("skald-vcs", {public = true})
        add_defines("LOKI_HAS_SKALD", {public = true})
        after_build(function (target)
            local sdk = target:pkg("skald-vcs")
            os.cp(path.join(sdk:installdir(), "bin", "skald_vcs.dll"), target:targetdir())
            os.cp(path.join(sdk:installdir(), "share", "SKALD-LICENSE"), target:targetdir())
        end)
    end
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("crypt32", "advapi32", {public = true})
        add_cxflags("/utf-8", {public = true, tools = {"cl", "clang_cl"}})
    end

if has_config("gui") then
target("loki-sync")
    set_kind("binary")
    set_basename("loki-client")
    add_deps("loki-core")
    on_load(function (target)
        target:add("defines", 'LOKI_CLIENT_VERSION="' .. target:version() .. '"')
    end)
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
