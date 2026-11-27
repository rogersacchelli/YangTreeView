// YangTreeView - Notepad++ plugin
// Plugins > YANG Tree View > Show YANG tree (Ctrl+Alt+Y)
// Renders the module in the active tab (unsaved edits included) as an
// interactive HTML tree, shown in a docked panel (WebView2) or the default browser.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "dock_panel.hpp"
#include "npp_plugin_api.h"
#include "udl_xml.h"
#include "yang_core.hpp"

namespace fs = std::filesystem;
using namespace yang2html;

static const wchar_t kPluginName[] = L"YANG Tree View";
static const wchar_t kIniName[] = L"YangTreeView.ini";

static NppData g_npp;
static HINSTANCE g_hInst = nullptr;
static ShortcutKey g_skShow = {true, true, false, 'Y'};
static FuncItem g_funcs[9];
static const int kShowTreeIdx = 0;  // also the docking dlgID Notepad++ reruns on startup
static const int kDockIdx = 3;      // "Use docked panel" (checkable)
static const int kFollowIdx = 4;    // "Panel follows active tab" (checkable)

// Settings, stored in YangTreeView.ini next to the search paths.
struct Settings {
    bool docked = true;  // panel=docked|browser: what Show YANG tree opens
    bool follow = true;  // follow=yes|no: refresh on save / follow the active .yang tab
};
static Settings g_set;
static bool g_ready = false;        // NPPN_READY received

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
    "# YANG Tree View - search paths for imported / included modules, and settings\r\n"
    "#\r\n"
    "# One folder per line (searched recursively). The folder of the current\r\n"
    "# file is always searched (3 levels deep), and so is the YANG_MODPATH\r\n"
    "# environment variable (';' separated).\r\n"
    "#\r\n"
    "# Examples:\r\n"
    "# C:\\yang\\openconfig\\release\\models\r\n"
    "# C:\\yang\\nokia\\7x50_YangModels\r\n"
    "# \\\\fileserver\\share\\yang\r\n"
    "#\r\n"
    "# Settings (also in the Plugins > YANG Tree View menu; saving this file applies them):\r\n"
    "#   panel=docked   Show YANG tree opens the panel docked in Notepad++\r\n"
    "#   panel=browser  Show YANG tree opens the default browser\r\n"
    "#   follow=yes     the panel refreshes on save and follows the active .yang tab\r\n"
    "#   follow=no      the panel only changes on Show YANG tree\r\n"
    "panel=docked\r\n"
    "follow=yes\r\n";

static std::string trimLower(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    s = b == std::string::npos ? "" : s.substr(b);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// "panel=..." / "follow=..." (anything else is a search path, as in 1.0)
static bool settingLine(const std::string& line, std::string& key, std::string& value) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = trimLower(line.substr(0, eq));
    if (key != "panel" && key != "follow") return false;
    value = trimLower(line.substr(eq + 1));
    return true;
}

static std::vector<std::string> iniLines() {
    std::vector<std::string> out;
    std::ifstream in(configIni(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (out.empty() && line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);  // BOM
        out.push_back(line);
    }
    return out;
}

static void loadSettings() {
    g_set = Settings{};
    for (const auto& raw : iniLines()) {
        std::string t = trimLower(raw), k, v;
        if (t.empty() || t[0] == '#' || t[0] == ';' || !settingLine(t, k, v)) continue;
        bool no = v == "no" || v == "false" || v == "0" || v == "off";
        if (k == "panel") g_set.docked = v != "browser";
        else if (k == "follow") g_set.follow = !no;
    }
}

// Sets key=value in the ini, keeping everything else (creates it from the template).
static void saveSetting(const char* key, const char* value) {
    fs::path ini = configIni();
    std::error_code ec;
    std::vector<std::string> lines;
    if (fs::exists(ini, ec)) {
        lines = iniLines();
    } else {
        std::string t = kDefaultIni;
        for (size_t p = 0, q; p < t.size(); p = q + 1) {
            q = t.find('\n', p);
            if (q == std::string::npos) q = t.size();
            lines.push_back(t.substr(p, q - p));
        }
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
    }
    bool done = false;
    for (auto& l : lines) {
        std::string t = trimLower(l), k, v;
        if (!t.empty() && t[0] != '#' && t[0] != ';' && settingLine(t, k, v) && k == key) {
            l = std::string(key) + "=" + value + "\r";
            done = true;
        }
    }
    if (!done) lines.push_back(std::string(key) + "=" + value + "\r");
    fs::create_directories(ini.parent_path(), ec);
    std::ofstream o(ini, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) {
        o << l;
        if (l.empty() || l.back() != '\r') o << '\r';
        o << '\n';
    }
}

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
        std::string k, v;
        if (settingLine(line, k, v)) continue;
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

static void logLine(const std::wstring& text) {
    if (const wchar_t* log = selftestLog()) {
        std::ofstream o(fs::path(log), std::ios::binary | std::ios::app);
        o << narrow(text) << "\n";
    }
}

static fs::path pagesDir() {
    wchar_t tmp[MAX_PATH + 1] = {0};
    GetTempPathW(MAX_PATH, tmp);
    return fs::path(tmp) / L"yang2html";
}

static bool processElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

// WebView2 profile. Not next to notepad++.exe (the default; read-only under Program Files)
// and not in the roaming plugin config folder (the cache would roam). An elevated
// Notepad++ cannot share a profile with a normal one, hence the separate folder.
static fs::path webViewDataDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)(sizeof buf / sizeof buf[0]));
    fs::path base = (n > 0 && n < sizeof buf / sizeof buf[0]) ? fs::path(buf) : pagesDir();
    return base / L"YangTreeView" / (processElevated() ? L"WebView2-admin" : L"WebView2");
}

static void openInBrowser(const fs::path& page) {
    if (selftestLog()) { logLine(L"[Browser] " + page.wstring()); return; }  // CI: no browser
    HINSTANCE h = ShellExecuteW(g_npp._nppHandle, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) msg(L"Tree written to:\n" + page.wstring() + L"\n\nbut no browser could be started.", MB_ICONWARNING);
}

static bool writeFile(const fs::path& out, const std::string& data) {
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(data.data(), (std::streamsize)data.size());
    return (bool)o;
}

// Page shown in the panel when it is restored at startup but the active tab
// doesn't render (not YANG, or a syntax error).
static std::string placeholderHtml(const std::string& why) {
    std::string w;
    for (char c : why) {
        if (c == '<') w += "&lt;"; else if (c == '>') w += "&gt;"; else if (c == '&') w += "&amp;"; else w += c;
    }
    return "<!DOCTYPE html><html><head><meta charset='utf-8'><title>YANG Tree View</title><style>"
           ":root{--bg:#fff;--fg:#1d2330;--muted:#6b7280}html.dark{--bg:#1e1f22;--fg:#d7dae0;--muted:#8b919c}"
           "body{margin:0;padding:24px;background:var(--bg);color:var(--fg);font:13px/1.5 'Segoe UI',system-ui,sans-serif}"
           "kbd{font:12px Consolas,monospace;border:1px solid var(--muted);border-radius:3px;padding:0 4px}"
           "p.why{color:var(--muted);white-space:pre-wrap}</style><script>"
           "var r=document.documentElement,q=new URLSearchParams(location.search).get('theme');"
           "function s(d){r.classList.toggle('dark',!!d)}"
           "if(q)s(q==='dark');else try{var t=localStorage.getItem('yang2html-theme');"
           "s(t==='dark'||(!t&&matchMedia('(prefers-color-scheme: dark)').matches))}catch(e){}"
           "if(window.chrome&&chrome.webview)chrome.webview.addEventListener('message',function(e){"
           "if(e.data&&e.data.type==='theme')s(e.data.dark)});</script></head><body>"
           "<p>Open a YANG module and press <kbd>Ctrl+Alt+Y</kbd> to show its tree here.</p>"
           "<p class='why'>" + w + "</p></body></html>";
}

// Renders the active tab into <temp>\yang2html\<name>.html.
// quiet: no message boxes and no caret jump (panel restored at startup).
static size_t g_shownHash = 0;  // hash of the page last shown in the panel
static size_t g_lastHash = 0;   // hash of the page last rendered
static fs::path g_shownPage;

// Renders the active tab into <temp>\yang2html\<name>.html.
// quiet: no message boxes and no caret jump (automatic refresh, panel restored at startup).
// unchanged (optional): set when the page equals the one last shown in the panel.
static bool renderActiveTab(bool quiet, fs::path& out, std::wstring& caption, std::string* error = nullptr,
                            bool* unchanged = nullptr) {
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
    RenderOptions ro;
    ro.includePaths = true;    // lets the panel open a node's file at its line
    ro.includeNetconf = true;  // NETCONF request builder in the panel
    RenderResult r = render(text, src, dirs, ro);
    SetCursor(old);

    if (!r.ok) {
        if (error) *error = r.error;
        if (!quiet) {
            if (r.errorLine > 0) SendMessageW(sci, SCI_GOTOLINE, (WPARAM)(r.errorLine - 1), 0);
            msg(L"Could not parse this YANG module:\n\n" + widen(r.error), MB_ICONERROR);
        }
        return false;
    }

    std::wstring stem = src.stem().wstring();
    if (stem.empty()) stem = widen(r.module);
    out = pagesDir() / (sanitize(stem) + L".html");
    caption = widen(r.module);
    size_t h = std::hash<std::string>{}(r.html);
    if (unchanged) *unchanged = (h == g_shownHash && out == g_shownPage);
    if (unchanged && *unchanged) return true;  // the panel already shows exactly this
    if (!writeFile(out, r.html)) {
        if (error) *error = "cannot write " + narrow(out.wstring());
        if (!quiet) msg(L"Cannot write " + out.wstring(), MB_ICONERROR);
        return false;
    }
    g_lastHash = h;
    return true;
}

static void onDockFailure(const std::wstring& why);
static void onPanelMessage(const std::wstring& m);
static bool onPanelKey(UINT vk, bool ctrl, bool alt, bool shift);

static void configureDock() {
    static bool done = false;
    if (done) return;
    done = true;
    dock::Config c;
    c.npp = g_npp;
    c.module = g_hInst;
    c.dlgId = kShowTreeIdx;
    c.pagesDir = pagesDir();
    // the self-test gets its own WebView2 profile: a profile in use by Notepad++ (another
    // program) can't be shared and fails with ERROR_INVALID_STATE
    c.userDataDir = selftestLog() ? pluginsConfigDir() / L"WebView2-selftest" : webViewDataDir();
    // self-test: no WebView2 unless explicitly requested
    c.createWebView = !selftestLog() || _wgetenv(L"YANGTREEVIEW_SELFTEST_WEBVIEW");
    c.log = logLine;
    c.onFailure = onDockFailure;
    c.onMessage = onPanelMessage;
    c.onAccelerator = onPanelKey;
    dock::configure(c);
}

static fs::path g_lastPage;  // for the browser fallback

static void onDockFailure(const std::wstring& why) {
    static bool told = false;
    if (!g_lastPage.empty()) openInBrowser(g_lastPage);
    if (!told) {
        told = true;
        msg(L"The docked viewer could not start (" + why + L").\n\nThe tree was opened in your browser instead.",
            MB_ICONWARNING);
    }
}

// Messages posted by the page in the panel.
static bool g_following = true;  // automatic refresh; off while we open a file ourselves
static bool g_closing = false;   // Notepad++ is closing its files (no refreshes)

// "goto<TAB>path<TAB>line" from the details pane: open the file at that line.
static void gotoSource(const std::wstring& path, int line) {
    fs::path p(path);
    std::error_code ec;
    if (!p.is_absolute() || line < 1 || !fs::is_regular_file(p, ec)) return;
    wchar_t cur[MAX_PATH * 4] = {0};
    SendMessageW(g_npp._nppHandle, NPPM_GETFULLCURRENTPATH, (WPARAM)(sizeof cur / sizeof cur[0]), (LPARAM)cur);
    if (!fs::equivalent(fs::path(cur), p, ec)) {
        g_following = false;  // keep the panel on the module being browsed
        LRESULT ok = SendMessageW(g_npp._nppHandle, NPPM_DOOPEN, 0, (LPARAM)p.c_str());
        g_following = true;
        if (!ok) return;
    }
    HWND sci = currentScintilla();
    int l = line - 1;
    SendMessageW(sci, SCI_ENSUREVISIBLEENFORCEPOLICY, (WPARAM)l, 0);  // unfold
    // show the line in the upper third of the view, caret on it
    int onScreen = (int)SendMessageW(sci, SCI_LINESONSCREEN, 0, 0);
    int vis = (int)SendMessageW(sci, SCI_VISIBLEFROMDOCLINE, (WPARAM)l, 0);
    SendMessageW(sci, SCI_SETFIRSTVISIBLELINE, (WPARAM)std::max(0, vis - onScreen / 3), 0);
    SendMessageW(sci, SCI_GOTOLINE, (WPARAM)l, 0);
    SetFocus(sci);
}

// "newdoc<TAB>text" (NETCONF request builder): a new tab with the text, as XML.
static void openInNewTab(const std::wstring& text) {
    SendMessageW(g_npp._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
    HWND sci = currentScintilla();
    // the page uses \n; follow the new document's line endings
    int eol = (int)SendMessageW(sci, SCI_GETEOLMODE, 0, 0);  // 0 CRLF, 1 CR, 2 LF
    std::wstring t;
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') t += eol == 0 ? L"\r\n" : eol == 1 ? L"\r" : L"\n";
        else t += c;
    }
    std::string bytes;
    if ((int)SendMessageW(sci, SCI_GETCODEPAGE, 0, 0) == SC_CP_UTF8) {
        bytes = narrow(t);
    } else {  // ANSI document
        int n = WideCharToMultiByte(CP_ACP, 0, t.data(), (int)t.size(), nullptr, 0, nullptr, nullptr);
        bytes.assign((size_t)n, '\0');
        if (n) WideCharToMultiByte(CP_ACP, 0, t.data(), (int)t.size(), &bytes[0], n, nullptr, nullptr);
    }
    SendMessageW(sci, SCI_SETTEXT, 0, (LPARAM)bytes.c_str());
    SendMessageW(g_npp._nppHandle, NPPM_SETCURRENTLANGTYPE, 0, L_XML);
    SetFocus(sci);
}

static void onPanelMessage(const std::wstring& m) {
    if (m.rfind(L"newdoc\t", 0) == 0) {
        openInNewTab(m.substr(7));
    } else if (m.rfind(L"goto\t", 0) == 0) {
        size_t t2 = m.find(L'\t', 5);
        if (t2 != std::wstring::npos) gotoSource(m.substr(5, t2 - 5), _wtoi(m.c_str() + t2 + 1));
    } else if (m == L"popout") {
        if (!g_lastPage.empty()) openInBrowser(g_lastPage);
    } else if (m == L"focus-editor") {
        SetFocus(currentScintilla());
    }
}

static void cmdShowTree();

// Keys typed in the panel don't reach Notepad++'s shortcuts. Handle our own command's
// shortcut (as currently mapped in Shortcut Mapper) so it refreshes the tree from the panel.
static bool onPanelKey(UINT vk, bool ctrl, bool alt, bool shift) {
    ShortcutKey sk = {};
    if (!SendMessageW(g_npp._nppHandle, NPPM_GETSHORTCUTBYCMDID, (WPARAM)g_funcs[kShowTreeIdx]._cmdID, (LPARAM)&sk))
        return false;  // no shortcut assigned
    if (sk._key == 0 || vk != sk._key || ctrl != sk._isCtrl || alt != sk._isAlt || shift != sk._isShift) return false;
    dock::defer(cmdShowTree);
    return true;
}

// True if the docked viewer can be used; otherwise explains once why not.
static bool dockAvailable() {
    if (selftestLog() && !_wgetenv(L"YANGTREEVIEW_SELFTEST_WEBVIEW")) return true;  // panel-only test
    configureDock();  // the loader needs the plugin module handle (WebView2Loader.dll lookup)
    if (!dock::runtimeVersion().empty()) return true;
    static bool told = false;
    if (!told) {
        told = true;
        msg(L"The docked viewer needs the Microsoft Edge WebView2 Runtime, which was not found.\n\n"
            L"The tree opens in your browser instead. The runtime is part of Windows 11 and "
            L"available for Windows 10 from:\nhttps://developer.microsoft.com/microsoft-edge/webview2/",
            MB_ICONINFORMATION);
    }
    return false;
}

static bool g_restorePending = false;  // panel restored before NPPN_READY

static bool activeTabIsYangFile() {
    wchar_t pathBuf[MAX_PATH * 4] = {0};
    SendMessageW(g_npp._nppHandle, NPPM_GETFULLCURRENTPATH, (WPARAM)(sizeof pathBuf / sizeof pathBuf[0]),
                 (LPARAM)pathBuf);
    return _wcsicmp(fs::path(pathBuf).extension().c_str(), L".yang") == 0;
}

// quiet: panel restored at startup - never a message box, a caret jump or the browser;
// if the active tab doesn't render, the panel shows a placeholder page instead.
// Save / tab switch while the panel is visible: re-render quietly. Keeps the last good
// tree (with a banner) on errors, and doesn't reload when nothing changed.
static void autoRefresh() {
    if (!g_ready || !g_set.follow || !g_following || g_closing || !dock::isVisible() || !activeTabIsYangFile()) return;
    try {
        fs::path out;
        std::wstring caption;
        std::string error;
        bool unchanged = false;
        if (!renderActiveTab(true, out, caption, &error, &unchanged)) {
            dock::postStatus(L"Not updated: " + widen(error));
            return;
        }
        if (unchanged) { dock::postStatus(L""); return; }
        g_lastPage = out;
        configureDock();
        if (dock::show(out, caption)) { g_shownHash = g_lastHash; g_shownPage = out; }
    } catch (...) {
    }
}

static void showTreeInPanel(bool quiet) {
    fs::path out;
    std::wstring caption;
    std::string error;
    if (!renderActiveTab(quiet, out, caption, &error)) {
        if (!quiet) return;
        out = pagesDir() / L"_yangtreeview_start.html";
        caption.clear();
        if (!writeFile(out, placeholderHtml(activeTabIsYangFile() ? error : ""))) return;
    }
    g_lastPage = out;
    if (!dockAvailable()) {
        if (!quiet) openInBrowser(out);
        return;
    }
    configureDock();
    if (dock::show(out, caption)) { g_shownHash = g_lastHash; g_shownPage = out; }
    else if (!quiet) openInBrowser(out);
}

// NPPN_READY: the buffers are loaded now, so render what a restored panel should show.
static void finishRestore() {
    if (!g_restorePending) return;
    g_restorePending = false;
    try {
        showTreeInPanel(true);
    } catch (...) {
    }
}

// Ctrl+Alt+Y: show the tree in the docked panel (falls back to the browser).
static void cmdShowInBrowser();

static void cmdShowTree() {
    try {
        if (!g_set.docked) {             // panel=browser
            if (g_ready) cmdShowInBrowser();  // (never open a browser at startup)
            return;
        }
        // Notepad++ reruns this command at startup when the panel was open at exit, before
        // the files are loaded: show the panel with a placeholder now and render the
        // active tab once Notepad++ is ready.
        if (!g_ready) {
            g_restorePending = true;
            fs::path out = pagesDir() / L"_yangtreeview_start.html";
            if (!writeFile(out, placeholderHtml(""))) return;
            g_lastPage = out;
            if (!dockAvailable()) return;
            configureDock();
            dock::show(out, L"");
            return;
        }
        showTreeInPanel(false);
    } catch (const std::exception& e) {
        msg(L"Unexpected error: " + widen(e.what()), MB_ICONERROR);
    } catch (...) {
        msg(L"Unexpected error.", MB_ICONERROR);
    }
}

static void cmdShowInBrowser() {
    try {
        fs::path out;
        std::wstring caption;
        if (renderActiveTab(false, out, caption)) openInBrowser(out);
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

static void setCheck(int idx, bool on) {
    SendMessageW(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, (WPARAM)g_funcs[idx]._cmdID, on ? TRUE : FALSE);
}

static void applySettings() {
    setCheck(kDockIdx, g_set.docked);
    setCheck(kFollowIdx, g_set.follow);
    // browser mode: close the panel, so Notepad++ doesn't restore it at the next start
    if (!g_set.docked) dock::hide();
}

static void cmdToggleDock() {
    g_set.docked = !g_set.docked;
    saveSetting("panel", g_set.docked ? "docked" : "browser");
    applySettings();
}

static void cmdToggleFollow() {
    g_set.follow = !g_set.follow;
    saveSetting("follow", g_set.follow ? "yes" : "no");
    applySettings();
}

// NPPN_FILESAVED for YangTreeView.ini (edited via "Search paths..."): apply its settings.
static bool reloadSettingsIfIni(UINT_PTR bufferId) {
    wchar_t buf[MAX_PATH * 4] = {0};
    if (SendMessageW(g_npp._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, (WPARAM)bufferId, (LPARAM)buf) <= 0) return false;
    std::error_code ec;
    if (!fs::equivalent(fs::path(buf), configIni(), ec)) return false;
    loadSettings();
    applySettings();
    return true;
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
        L"tree in a panel docked to Notepad++ (Ctrl+Alt+Y) or in your browser. In the "
        L"panel, click a node's file : line to jump to its source.\n\n"
        L"Groupings used via 'uses' are resolved from the module, its submodules and "
        L"imported modules found in the file's folder, the folders listed under "
        L"'Search paths and settings...', and YANG_MODPATH.\n\n"
        L"Plugins > YANG Tree View > Install YANG syntax highlighting adds colouring "
        L"and folding for .yang files.\n\n"
        L"No Python or other runtime required.\n\nhttps://github.com/rogersacchelli/YangTreeView");
}

// ------------------------------------------------------------------------- //
// Notepad++ plugin interface
// ------------------------------------------------------------------------- //

static void setItem(int i, const wchar_t* name, PFUNCPLUGINCMD fn, ShortcutKey* sk, bool checked = false) {
    wcsncpy(g_funcs[i]._itemName, name, nbChar - 1);
    g_funcs[i]._itemName[nbChar - 1] = 0;
    g_funcs[i]._pFunc = fn;
    g_funcs[i]._init2Check = checked;
    g_funcs[i]._pShKey = sk;
}

extern "C" {

__declspec(dllexport) void setInfo(NppData data) {
    g_npp = data;
    loadSettings();
    setItem(kShowTreeIdx, L"Show YANG tree", cmdShowTree, &g_skShow);
    setItem(1, L"Show YANG tree in browser", cmdShowInBrowser, nullptr);
    setItem(2, L"", nullptr, nullptr);  // separator
    setItem(kDockIdx, L"Use docked panel", cmdToggleDock, nullptr, g_set.docked);
    setItem(kFollowIdx, L"Panel follows active tab", cmdToggleFollow, nullptr, g_set.follow);
    setItem(5, L"", nullptr, nullptr);  // separator
    setItem(6, L"Install YANG syntax highlighting", cmdInstallSyntax, nullptr);
    setItem(7, L"Search paths and settings...", cmdSearchPaths, nullptr);
    setItem(8, L"About", cmdAbout, nullptr);
}

__declspec(dllexport) const wchar_t* getName() { return kPluginName; }

__declspec(dllexport) FuncItem* getFuncsArray(int* nbF) {
    *nbF = (int)(sizeof g_funcs / sizeof g_funcs[0]);
    return g_funcs;
}

__declspec(dllexport) void beNotified(SCNotification* n) {
    if (!n || n->nmhdr.hwndFrom != g_npp._nppHandle) return;
    switch (n->nmhdr.code) {
        case NPPN_READY: g_ready = true; finishRestore(); break;
        case NPPN_SHUTDOWN: dock::shutdown(); break;
        case NPPN_DARKMODECHANGED: dock::themeChanged(); break;
        case NPPN_BEFORESHUTDOWN: g_closing = true; break;
        case NPPN_CANCELSHUTDOWN: g_closing = false; break;
        case NPPN_FILESAVED:
            if (reloadSettingsIfIni(n->nmhdr.idFrom)) break;
            autoRefresh();
            break;
        case NPPN_BUFFERACTIVATED: autoRefresh(); break;
    }
}

__declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

__declspec(dllexport) BOOL isUnicode() { return TRUE; }

}  // extern "C"

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_hInst = h;
    return TRUE;
}
