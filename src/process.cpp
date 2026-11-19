#include "process.hpp"

#include <array>
#include <filesystem>
#include <cwchar>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace loki {

#ifdef _WIN32
namespace {
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 argument.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size);
    return result;
}

std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else {
            result.append(slashes * (ch == L'"' ? 2 : 1) + (ch == L'"'), L'\\');
            result.push_back(ch);
            slashes = 0;
        }
    }
    return result + std::wstring(slashes * 2, L'\\') + L"\"";
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Windows argument.");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::map<std::string, std::string>& overrides, bool capture, bool capture_errors) {
    std::wstring executable;
    if (const auto* path = _wgetenv(L"PATH")) {
        const std::wstring paths(path);
        size_t start = 0;
        while (start < paths.size()) {
            const auto end = paths.find(L';', start);
            const auto directory = std::filesystem::path(paths.substr(start, end - start));
            const auto candidate = directory / (wide(arguments.front()) + L".exe");
            std::error_code ignored;
            if (directory.is_absolute() && std::filesystem::is_regular_file(candidate, ignored)) {
                executable = candidate.native();
                break;
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }
    if (executable.empty()) throw std::runtime_error("Could not find Git on PATH.");
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += quote(wide(argument));
    }
    auto compare = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
    std::map<std::wstring, std::wstring, decltype(compare)> variables(compare);
    auto* environment = GetEnvironmentStringsW();
    if (!environment) throw std::runtime_error("Could not read the process environment.");
    for (auto* p = environment; *p; p += wcslen(p) + 1) {
        const std::wstring entry(p);
        const auto split = entry.find(L'=', 1);
        if (split != std::wstring::npos) variables[entry.substr(0, split)] = entry.substr(split + 1);
    }
    FreeEnvironmentStringsW(environment);
    for (const auto& [key, value] : overrides) variables[wide(key)] = wide(value);
    std::wstring block;
    for (const auto& [key, value] : variables) {
        block += key + L'=' + value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Handle reader, writer, input;
    if (capture) {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        if (!CreatePipe(&reader.value, &writer.value, &attributes, 0)
            || !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error("Could not open a Git output pipe.");
        }
        startup.dwFlags = STARTF_USESTDHANDLES;
        if (capture_errors) {
            input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open Git input.");
        }
        startup.hStdInput = capture_errors ? input.value : GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdError = capture_errors ? writer.value : GetStdHandle(STD_ERROR_HANDLE);
        startup.hStdOutput = writer.value;
    }
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, capture, CREATE_UNICODE_ENVIRONMENT | (capture ? CREATE_NO_WINDOW : 0),
                        block.data(), nullptr, &startup, &info)) {
        throw std::runtime_error("Could not start Git. Install Git and put it on PATH.");
    }
    const Handle process{info.hProcess}, thread{info.hThread};
    ProcessResult result{1, {}};
    if (capture) {
        CloseHandle(writer.value);
        writer.value = nullptr;
        std::array<char, 4096> buffer{};
        DWORD length;
        while (ReadFile(reader.value, buffer.data(), DWORD(buffer.size()), &length, nullptr) && length) {
            result.output.append(buffer.data(), length);
        }
    }
    WaitForSingleObject(process.value, INFINITE);
    DWORD code;
    if (GetExitCodeProcess(process.value, &code)) result.status = int(code);
    return result;
}

#else

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::map<std::string, std::string>& overrides, bool capture, bool capture_errors) {
    auto variables = overrides;
    for (auto** p = environ; *p; ++p) {
        const std::string entry(*p);
        const auto split = entry.find('=');
        if (split != std::string::npos) variables.try_emplace(entry.substr(0, split), entry.substr(split + 1));
    }
    std::vector<std::string> environment;
    for (const auto& [key, value] : variables) environment.push_back(key + '=' + value);
    std::vector<char*> argv, envp;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    for (auto& value : environment) envp.push_back(value.data());
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    int pipefd[2]{-1, -1};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("Could not prepare Git.");
    int setup = 0;
    if (capture) {
        if (pipe(pipefd) != 0) setup = errno;
        if (!setup) setup = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        if (!setup && capture_errors) setup = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        if (!setup) setup = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        if (!setup && pipefd[1] != STDOUT_FILENO) setup = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    }
    pid_t pid;
    const int error = setup ? setup : posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (error) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        throw std::runtime_error("Could not start Git. Install Git and put it on PATH.");
    }
    ProcessResult result{1, {}};
    if (capture) {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto length = read(pipefd[0], buffer.data(), buffer.size());
            if (length < 0 && errno == EINTR) continue;
            if (length <= 0) break;
            result.output.append(buffer.data(), size_t(length));
        }
        close(pipefd[0]);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited >= 0 && WIFEXITED(status)) result.status = WEXITSTATUS(status);
    return result;
}
#endif

}
