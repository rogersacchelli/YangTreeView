// yang_core.hpp - YANG parser + collapsible HTML tree generator (portable C++17)
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace yang2html {

namespace fs = std::filesystem;

struct SearchDir {
    fs::path dir;
    int maxDepth;   // recursion depth (0 = only this folder)
};

struct RenderResult {
    bool ok = false;
    std::string html;       // complete page (on success)
    std::string json;       // embedded payload (for tests)
    std::string module;     // module name
    std::string error;      // UTF-8 message (on failure)
    int errorLine = 0;      // 1-based line in the input, 0 if unknown
};

// text: UTF-8 YANG source of the main module.
// sourcePath: where it came from (used for file names in the UI).
RenderResult render(const std::string& text, const fs::path& sourcePath,
                    const std::vector<SearchDir>& dirs);

std::string errorHtml(const std::string& msg);

const char* version();

}  // namespace yang2html
