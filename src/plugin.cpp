// YangTreeView - Notepad++ plugin
// Plugins > YANG Tree View > Show YANG tree (Ctrl+Alt+Y)
// Renders the module in the active tab (unsaved edits included) as an
// interactive HTML tree and opens it in the default browser.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "npp_plugin_api.h"
#include "udl_xml.h"
#include "yang_core.hpp"

namespace fs = std::filesystem;
using namespace yang2html;

static const wchar_t kPluginName[] = L"YANG Tree View";
static const wchar_t kIniName[] = L"YangTreeView.ini";

static NppData g_npp;
static ShortcutKey g_skShow = {true, true, false, 'Y'};
static FuncItem g_funcs[5];

// ------------------------------------------------------------------------- //
// helpers
// ------------------------------------------------------------------------- //

static std::wstring widen(const std::string& s, UINT cp = CP_UTF8) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static HWND currentScintilla() {
    int which = -1;
    SendMessageW(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
    return which == 1 ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}

// Self-test mode (CI has no interactive desktop): when YANGTREEVIEW_SELFTEST names a
// file, dialogs are appended to it instead of shown (answer: IDYES), and no browser
// is launched.
static const wchar_t* selftestLog() {
    const wchar_t* p = _wgetenv(L"YANGTREEVIEW_SELFTEST");
    return (p && *p) ? p : nullptr;
}

static int box(const std::wstring& text, UINT flags) {
    if (const wchar_t* log = selftestLog()) {
        std::ofstream o(fs::path(log), std::ios::binary | std::ios::app);
        std::string line = narrow(text);
        for (auto& c : line)
            if (c == '\n' || c == '\r') c = ' ';
        o << "[MessageBox] " << line << "\n";
        return IDYES;
    }
    return MessageBoxW(g_npp._nppHandle, text.c_str(), kPluginName, flags);
}

static void msg(const std::wstring& text, UINT icon = MB_ICONINFORMATION) { box(text, MB_OK | icon); }

static fs::path pluginsConfigDir() {
    wchar_t buf[MAX_PATH * 4] = {0};
    SendMessageW(g_npp._nppHandle, NPPM_GETPLUGINSCONFIGDIR, (WPARAM)(sizeof buf / sizeof buf[0]), (LPARAM)buf);
    return fs::path(buf);
}

static fs::path configIni() { return pluginsConfigDir() / kIniName; }

// Folder holding config.xml, userDefineLangs\ etc. (AppData, -settingsDir, cloud or portable)
static fs::path nppSettingsDir() {
    wchar_t buf[MAX_PATH * 4] = {0};
    LRESULT n = SendMessageW(g_npp._nppHandle, NPPM_GETNPPSETTINGSDIRPATH, (WPARAM)(sizeof buf / sizeof buf[0]),
                             (LPARAM)buf);
    if (n > 0 && buf[0]) return fs::path(buf);
    // older Notepad++: plugins config dir is <settings>\plugins\Config
    return pluginsConfigDir().parent_path().parent_path();
}

static const char kDefaultIni[] =
    "# YANG Tree View - search paths for imported / included modules\r\n"
    "#\r\n"
    "# One folder per line (searched recursively). The folder of the current\r\n"
    "# file is always searched (3 levels deep), and so is the YANG_MODPATH\r\n"
    "# environment variable (';' separated).\r\n"
    "#\r\n"
    "# Examples:\r\n"
    "# C:\\yang\\openconfig\\release\\models\r\n"
    "# C:\\yang\\nokia\\7x50_YangModels\r\n"
    "# \\\\fileserver\\share\\yang\r\n";

static std::vector<fs::path> iniPaths() {
    std::vector<fs::path> out;
    std::ifstream in(configIni(), std::ios::binary);
    if (!in) return out;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);  // BOM
        first = false;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line[0] == '#' || line[0] == ';') continue;
        if (line.rfind("path=", 0) == 0) line = line.substr(5);
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"') line = line.substr(1, line.size() - 2);
        out.push_back(fs::path(widen(line)));
    }
    return out;
}

static std::wstring sanitize(std::wstring s) {
    for (auto& c : s)
        if (wcschr(L"<>:\"/\\|?* ", c) || c < 32) c = L'_';
    return s.empty() ? L"module" : s;
}

// ------------------------------------------------------------------------- //
// commands
// ------------------------------------------------------------------------- //

static void cmdShowTree() {
    try {
        HWND sci = currentScintilla();
        LRESULT len = SendMessageW(sci, SCI_GETLENGTH, 0, 0);
        std::string text((size_t)len + 1, '\0');
        SendMessageW(sci, SCI_GETTEXT, (WPARAM)len + 1, (LPARAM)&text[0]);
        text.resize((size_t)len);
        if ((int)SendMessageW(sci, SCI_GETCODEPAGE, 0, 0) != SC_CP_UTF8)
            text = narrow(widen(text, CP_ACP));  // ANSI buffer -> UTF-8

        wchar_t pathBuf[MAX_PATH * 4] = {0};
        SendMessageW(g_npp._nppHandle, NPPM_GETFULLCURRENTPATH, (WPARAM)(sizeof pathBuf / sizeof pathBuf[0]),
                     (LPARAM)pathBuf);
        fs::path src(pathBuf);

        std::vector<SearchDir> dirs;
        if (src.is_absolute() && src.has_parent_path()) dirs.push_back({src.parent_path(), 3});
        for (const auto& p : iniPaths()) dirs.push_back({p, 50});
        if (const wchar_t* mp = _wgetenv(L"YANG_MODPATH")) {
            std::wstring s(mp), cur;
            for (size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == L';') { if (!cur.empty()) dirs.push_back({fs::path(cur), 50}); cur.clear(); }
                else cur += s[i];
            }
        }

        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        RenderResult r = render(text, src, dirs);
        SetCursor(old);

        if (!r.ok) {
            if (r.errorLine > 0) SendMessageW(sci, SCI_GOTOLINE, (WPARAM)(r.errorLine - 1), 0);
            msg(L"Could not parse this YANG module:\n\n" + widen(r.error), MB_ICONERROR);
            return;
        }

        wchar_t tmp[MAX_PATH + 1] = {0};
        GetTempPathW(MAX_PATH, tmp);
        fs::path outDir = fs::path(tmp) / L"yang2html";
        std::error_code ec;
        fs::create_directories(outDir, ec);
        std::wstring stem = src.stem().wstring();
        if (stem.empty()) stem = widen(r.module);
        fs::path out = outDir / (sanitize(stem) + L".html");
        {
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            if (!o) { msg(L"Cannot write " + out.wstring(), MB_ICONERROR); return; }
            o.write(r.html.data(), (std::streamsize)r.html.size());
        }
        if (selftestLog()) return;  // CI: don't launch a browser
        HINSTANCE h = ShellExecuteW(g_npp._nppHandle, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) msg(L"Tree written to:\n" + out.wstring() + L"\n\nbut no browser could be started.", MB_ICONWARNING);
    } catch (const std::exception& e) {
        msg(L"Unexpected error: " + widen(e.what()), MB_ICONERROR);
    } catch (...) {
        msg(L"Unexpected error.", MB_ICONERROR);
    }
}

static bool fileDefinesYang(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s.find("<userlang name=\"yang") != std::string::npos;
}

static void cmdInstallSyntax() {
    try {
        fs::path settings = nppSettingsDir();
        fs::path dir = settings / L"userDefineLangs";
        fs::path target = dir / L"YANG.xml";
        bool dark = SendMessageW(g_npp._nppHandle, NPPM_ISDARKMODEENABLED, 0, 0) != 0;
        std::error_code ec;

        // other definitions of a "YANG" language confuse the Language menu
        std::wstring others;
        if (fs::is_directory(dir, ec))
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path() == target || e.path().extension() != L".xml") continue;
                if (fileDefinesYang(e.path())) others += L"\n  " + e.path().wstring();
            }
        if (fs::exists(settings / L"userDefineLang.xml", ec) && fileDefinesYang(settings / L"userDefineLang.xml"))
            others += L"\n  " + (settings / L"userDefineLang.xml").wstring() +
                      L"  (remove it via Language > User Defined Language > Define your language)";

        if (fs::exists(target, ec) &&
            box(L"YANG syntax highlighting is already installed:\n" + target.wstring() + L"\n\nReplace it with the " +
                    (dark ? L"dark" : L"light") + L" colour scheme?",
                MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;

        fs::create_directories(dir, ec);
        {
            std::ofstream o(target, std::ios::binary | std::ios::trunc);
            if (!o) {
                msg(L"Cannot write " + target.wstring() +
                        L"\n\nIf Notepad++ is installed in a protected folder with local settings, "
                        L"copy the file manually or run Notepad++ as administrator once.",
                    MB_ICONERROR);
                return;
            }
            const char* xml = dark ? kUdlDark : kUdlLight;
            o.write(xml, (std::streamsize)strlen(xml));
        }
        std::wstring text = std::wstring(L"YANG syntax highlighting installed (") + (dark ? L"dark" : L"light") +
                            L" colours):\n" + target.wstring() +
                            L"\n\nRestart Notepad++ to activate it. Files ending in .yang will then be "
                            L"highlighted automatically, with folding on { } blocks.\n\n"
                            L"Tip: after switching between light and dark mode, run this command again.";
        if (!others.empty())
            text += L"\n\nNote: other YANG language definitions were found and may conflict:" + others;
        msg(text, others.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
    } catch (const std::exception& e) {
        msg(L"Unexpected error: " + widen(e.what()), MB_ICONERROR);
    }
}

static void cmdSearchPaths() {
    fs::path ini = configIni();
    std::error_code ec;
    if (!fs::exists(ini, ec)) {
        fs::create_directories(ini.parent_path(), ec);
        std::ofstream o(ini, std::ios::binary);
        o << kDefaultIni;
    }
    SendMessageW(g_npp._nppHandle, NPPM_DOOPEN, 0, (LPARAM)ini.c_str());
}

static void cmdAbout() {
    msg(std::wstring(L"YANG Tree View ") + widen(version()) +
        L"\n\nRenders the YANG module in the active tab as an interactive, collapsible "
        L"tree in your browser (Ctrl+Alt+Y).\n\n"
        L"Groupings used via 'uses' are resolved from the module, its submodules and "
        L"imported modules found in the file's folder, the folders listed under "
        L"'Search paths...', and YANG_MODPATH.\n\n"
        L"Plugins > YANG Tree View > Install YANG syntax highlighting adds colouring "
        L"and folding for .yang files.\n\n"
        L"No Python or other runtime required.\n\nhttps://github.com/rogersacchelli/YangTreeView");
}

// ------------------------------------------------------------------------- //
// Notepad++ plugin interface
// ------------------------------------------------------------------------- //

static void setItem(int i, const wchar_t* name, PFUNCPLUGINCMD fn, ShortcutKey* sk) {
    wcsncpy(g_funcs[i]._itemName, name, nbChar - 1);
    g_funcs[i]._itemName[nbChar - 1] = 0;
    g_funcs[i]._pFunc = fn;
    g_funcs[i]._init2Check = false;
    g_funcs[i]._pShKey = sk;
}

extern "C" {

__declspec(dllexport) void setInfo(NppData data) {
    g_npp = data;
    setItem(0, L"Show YANG tree", cmdShowTree, &g_skShow);
    setItem(1, L"", nullptr, nullptr);  // separator
    setItem(2, L"Install YANG syntax highlighting", cmdInstallSyntax, nullptr);
    setItem(3, L"Search paths...", cmdSearchPaths, nullptr);
    setItem(4, L"About", cmdAbout, nullptr);
}

__declspec(dllexport) const wchar_t* getName() { return kPluginName; }

__declspec(dllexport) FuncItem* getFuncsArray(int* nbF) {
    *nbF = (int)(sizeof g_funcs / sizeof g_funcs[0]);
    return g_funcs;
}

__declspec(dllexport) void beNotified(SCNotification*) {}

__declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

__declspec(dllexport) BOOL isUnicode() { return TRUE; }

}  // extern "C"

BOOL APIENTRY DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
