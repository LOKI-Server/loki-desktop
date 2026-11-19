#pragma once

#include <map>
#include <string>
#include <vector>

namespace loki {

struct ProcessResult {
    int status;
    std::string output;
};

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::map<std::string, std::string>& overrides = {}, bool capture = false, bool capture_errors = false);
#ifdef _WIN32
std::string narrow(const std::wstring& text);
#endif

}
