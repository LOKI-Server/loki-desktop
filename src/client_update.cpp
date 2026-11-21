#include "client_update.hpp"
#include <array>
#include <charconv>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include "api.hpp"
#include "sha256.hpp"
#include "store.hpp"
#include <curl/curl.h>
#include <fstream>
#include <random>
#include <windows.h>
#include <wincrypt.h>
#endif

namespace loki {
namespace {
constexpr std::string_view release_root = "https://github.com/LOKI-Server/loki-desktop/releases/download/";
constexpr std::uint64_t max_archive = 128 * 1024 * 1024;

std::optional<std::array<unsigned, 3>> version_number(std::string_view text) {
    std::array<unsigned, 3> parts{};
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto dot = text.find('.');
        if ((dot == text.npos) != (i == parts.size() - 1)) return std::nullopt;
        const auto segment = text.substr(0, dot);
        if (segment.empty() || (segment.size() > 1 && segment.front() == '0')) return std::nullopt;
        const auto end = segment.data() + segment.size();
        const auto parsed = std::from_chars(segment.data(), end, parts[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
        if (dot != text.npos) text.remove_prefix(dot + 1);
    }
    return parts;
}

std::string archive_name(const std::string& version) {
    return "LOKI-Client-" + version + "-windows-x64.zip";
}

bool valid_digest(std::string_view digest) {
    return digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == digest.npos;
}
}

std::optional<ClientUpdate> select_client_update(std::string_view metadata, std::string_view current) {
    const auto release = nlohmann::json::parse(metadata);
    const auto tag = release.at("tag_name").get<std::string>();
    const auto version = tag.starts_with('v') ? tag.substr(1) : std::string{};
    const auto latest = version_number(version), installed = version_number(current);
    if (!installed) throw std::runtime_error("The desktop version is invalid.");
    if (!latest || *latest <= *installed || release.value("draft", false) || release.value("prerelease", false))
        return std::nullopt;
    const auto asset_name = archive_name(version);
    const auto url = std::string(release_root) + tag + "/" + asset_name;
    for (const auto& asset : release.at("assets")) {
        if (asset.value("name", std::string{}) != asset_name) continue;
        if (!asset.contains("digest") || !asset["digest"].is_string())
            throw std::runtime_error("The release ZIP has no SHA-256 digest.");
        const auto digest = asset["digest"].get<std::string>();
        const auto size = asset.at("size").get<std::uint64_t>();
        if (asset.at("browser_download_url") != url || !digest.starts_with("sha256:")
            || !valid_digest(std::string_view(digest).substr(7)) || size == 0 || size > max_archive)
            throw std::runtime_error("The desktop update has invalid release metadata.");
        return ClientUpdate{version, url, digest.substr(7), size};
    }
    throw std::runtime_error("The new release has no Windows x64 ZIP.");
}

#ifdef _WIN32
namespace {
namespace fs = std::filesystem;
constexpr auto latest_url = "https://api.github.com/repos/LOKI-Server/loki-desktop/releases/latest";

struct Download {
    std::ostream& output;
    std::uint64_t remaining;
};

size_t write_download(char* data, size_t size, size_t count, void* opaque) {
    auto& download = *static_cast<Download*>(opaque);
    const auto bytes = size * count;
    if (bytes > download.remaining) return 0;
    download.output.write(data, static_cast<std::streamsize>(bytes));
    download.remaining -= bytes;
    return download.output ? bytes : 0;
}

std::uint64_t download(const std::string& url, std::ostream& output, std::uint64_t limit, bool redirects) {
    static const Api curl_initializer("http://localhost");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Cannot initialize the desktop update download.");
    Download sink{output, limit};
    const auto handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "LOKI-Desktop-Updater");
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, redirects ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, redirects ? 300L : 25L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_download);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
    const auto result = curl_easy_perform(handle);
    if (result != CURLE_OK) throw std::runtime_error(std::string("Desktop update download failed: ") + curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) throw std::runtime_error("GitHub did not return the requested update.");
    return limit - sink.remaining;
}

std::wstring quoted(std::wstring value) {
    std::wstring result{L"'"};
    for (const auto ch : value) {
        if (ch == L'\'') result += L'\'';
        result += ch;
    }
    return result + L"'";
}

std::wstring helper_script() {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(701), MAKEINTRESOURCEW(10));
    if (!resource) throw std::runtime_error("This build is missing its update helper.");
    const auto bytes = SizeofResource(nullptr, resource);
    const auto data = static_cast<const char*>(LockResource(LoadResource(nullptr, resource)));
    if (!data || !bytes) throw std::runtime_error("Cannot load the desktop update helper.");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, int(bytes), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid desktop update helper.");
    std::wstring code(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, int(bytes), code.data(), length);
    return code;
}

HANDLE start_helper(const fs::path& folder, const std::string& version, bool install) {
    std::wstring app(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, app.data(), DWORD(app.size()));
    if (!size || size == app.size()) throw std::runtime_error("Cannot locate the installed desktop executable.");
    app.resize(size);
    std::wstring system(MAX_PATH, L'\0');
    const auto system_size = GetSystemDirectoryW(system.data(), UINT(system.size()));
    if (!system_size || system_size >= system.size()) throw std::runtime_error("Cannot locate Windows PowerShell.");
    system.resize(system_size);
    const auto powershell = fs::path(system) / "WindowsPowerShell/v1.0/powershell.exe";
    const auto code = L"$stage=" + quoted(folder.native()) + L"\n$application=" + quoted(app)
                    + L"\n$version=" + quoted(path_from_utf8(version).native())
                    + L"\n$parentId=" + std::to_wstring(GetCurrentProcessId())
                    + (install ? L"\n$install=$true\n" : L"\n$install=$false\n") + helper_script();
    const auto raw = reinterpret_cast<const BYTE*>(code.data());
    const auto raw_size = DWORD(code.size() * sizeof(wchar_t));
    constexpr DWORD format = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD encoded_size = 0;
    if (!CryptBinaryToStringW(raw, raw_size, format, nullptr, &encoded_size))
        throw std::runtime_error("Cannot encode the desktop update helper.");
    std::wstring encoded(encoded_size, L'\0');
    if (!CryptBinaryToStringW(raw, raw_size, format, encoded.data(), &encoded_size))
        throw std::runtime_error("Cannot encode the desktop update helper.");
    encoded.resize(encoded_size);
    auto command = L"\"" + powershell.native() + L"\" -NoLogo -NoProfile -NonInteractive -EncodedCommand " + encoded;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, system.c_str(), &startup, &process))
        throw std::runtime_error("Cannot start the desktop update helper.");
    CloseHandle(process.hThread);
    return process.hProcess;
}

fs::path create_stage() {
    std::random_device random;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto name = "loki-update-" + std::to_string(random()) + "-" + std::to_string(random());
        const auto folder = fs::temp_directory_path() / name;
        if (fs::create_directory(folder)) return folder;
    }
    throw std::runtime_error("Cannot create the desktop update staging directory.");
}
}

std::optional<ClientUpdate> check_client_update(std::string_view current_version) {
    std::ostringstream buffer;
    download(latest_url, buffer, 512 * 1024, false);
    return select_client_update(buffer.str(), current_version);
}

PreparedClientUpdate::PreparedClientUpdate(fs::path folder, std::string version)
    : folder_(std::move(folder)), version_(std::move(version)) {}

PreparedClientUpdate::~PreparedClientUpdate() {
    if (folder_.empty()) return;
    std::error_code ignored;
    fs::remove_all(folder_, ignored);
}

void PreparedClientUpdate::install_after_exit() {
    const auto process = start_helper(folder_, version_, true);
    CloseHandle(process);
    folder_.clear();
}

std::shared_ptr<PreparedClientUpdate> prepare_client_update(const ClientUpdate& update) {
    if (!version_number(update.version) || !valid_digest(update.sha256) || update.size > max_archive
        || !update.size || update.url != std::string(release_root) + "v" + update.version + "/" + archive_name(update.version))
        throw std::runtime_error("Invalid desktop update.");
    auto prepared = std::shared_ptr<PreparedClientUpdate>(new PreparedClientUpdate(create_stage(), update.version));
    const auto archive = prepared->folder_ / "update.zip";
    std::ofstream out(archive, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot save the desktop update.");
    const auto bytes = download(update.url, out, update.size, true);
    out.close();
    if (!out || bytes != update.size || Sha256::of_file(archive) != update.sha256)
        throw std::runtime_error("The desktop update ZIP failed its SHA-256 check.");
    const auto process = start_helper(prepared->folder_, update.version, false);
    DWORD exit_code = 1;
    if (WaitForSingleObject(process, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(process, &exit_code);
    else { TerminateProcess(process, 1); WaitForSingleObject(process, 5000); }
    CloseHandle(process);
    if (exit_code != 0) {
        const auto error = read_text(prepared->folder_ / "update-error.txt");
        throw std::runtime_error(error.empty() ? "Cannot unpack the desktop update." : error);
    }
    return prepared;
}
#endif

}  // namespace loki
