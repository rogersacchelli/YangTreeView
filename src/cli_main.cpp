// yang2html CLI - standalone executable using the same core as the plugin.
//   yang2html module.yang [-p DIR]... [-o out.html] [--open]
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "yang_core.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace yang2html;

static std::string sanitize(std::string s) {
    for (char& c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '@' || c == '-' || c == '_' || (unsigned char)c >= 0x80)) c = '_';
    return s.empty() ? "module" : s;
}

static int run(const std::vector<fs::path>& args) {
    fs::path input, out;
    bool open = false;
    RenderOptions opts;
    opts.includeNetconf = true;  // NETCONF request builder (no local paths)
    std::vector<SearchDir> extra;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i].u8string();
        if ((a == "-p" || a == "--path") && i + 1 < args.size()) extra.push_back({args[++i], 50});
        else if ((a == "-o" || a == "--output") && i + 1 < args.size()) out = args[++i];
        else if (a == "--open") open = true;
        else if (a == "--no-netconf") opts.includeNetconf = false;
        // undocumented: embed the file paths the Notepad++ panel gets, for tests and
        // debugging; such pages reveal local folders
        else if (a == "--host-data") opts.includePaths = true;
        else if (a == "-h" || a == "--help") {
            std::printf("yang2html %s\nusage: yang2html FILE.yang [-p DIR]... [-o OUT.html] [--open] [--no-netconf]\n", version());
            return 0;
        } else input = args[i];
    }
    if (input.empty()) {
        std::fprintf(stderr, "usage: yang2html FILE.yang [-p DIR]... [-o OUT.html] [--open] [--no-netconf]\n");
        return 2;
    }
    std::error_code ec;
    fs::path abs = fs::absolute(input, ec);
    std::vector<SearchDir> dirs{{abs.parent_path(), 3}};
    dirs.insert(dirs.end(), extra.begin(), extra.end());
    if (const char* mp = std::getenv("YANG_MODPATH")) {
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string s(mp), cur;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == sep) { if (!cur.empty()) dirs.push_back({fs::u8path(cur), 50}); cur.clear(); }
            else cur += s[i];
        }
    }

    std::ifstream in(abs, std::ios::binary);
    if (!in) { std::fprintf(stderr, "error: cannot open %s\n", abs.u8string().c_str()); return 2; }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    RenderResult r = render(text, abs, dirs, opts);
    if (out.empty()) {
        fs::path d = fs::temp_directory_path(ec) / "yang2html";
        fs::create_directories(d, ec);
        out = d / fs::u8path(sanitize(abs.stem().u8string()) + ".html");
    }
    std::ofstream o(out, std::ios::binary);
    o << (r.ok ? r.html : errorHtml(abs.u8string() + "\n\n" + r.error));
    o.close();
    std::printf("%s\n", out.u8string().c_str());
    if (!r.ok) std::fprintf(stderr, "error: %s\n", r.error.c_str());
    if (open) {
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", out.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        std::string cmd = "xdg-open '" + out.u8string() + "' >/dev/null 2>&1 &";
        if (std::system(cmd.c_str())) {}
#endif
    }
    return r.ok ? 0 : 1;
}

#ifdef _WIN32
int main() {
    // use the wide command line so non-ASCII paths work
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(wargv[i]);
    LocalFree(wargv);
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#endif
