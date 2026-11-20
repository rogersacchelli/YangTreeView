// Fake Notepad++ host: loads YangTreeView.dll and drives it through the real
// plugin ABI with fake Notepad++ / Scintilla windows. Run under Wine or Windows.
//   fakehost.exe <dll> <yang file> <config dir> [<yang text file to use as buffer>]
// Exit code 0 = all checks passed.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/npp_plugin_api.h"

static std::string g_text;
static std::wstring g_path, g_cfg, g_opened;
static int g_gotoLine = -1;
static int g_dialogs = 0;
static int g_fail = 0;
#define CHECK(cond, what) do { if (cond) printf("  PASS %s\n", what); else { printf("  FAIL %s\n", what); ++g_fail; } fflush(stdout); } while (0)

static LRESULT CALLBACK NppProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case NPPM_GETCURRENTSCINTILLA: *(int*)l = 0; return TRUE;
        case NPPM_GETFULLCURRENTPATH: lstrcpynW((wchar_t*)l, g_path.c_str(), (int)w); return TRUE;
        case NPPM_GETPLUGINSCONFIGDIR: lstrcpynW((wchar_t*)l, g_cfg.c_str(), (int)w); return TRUE;
        case NPPM_DOOPEN: g_opened = (const wchar_t*)l; return TRUE;
        case NPPM_ISDARKMODEENABLED: return FALSE;
        case NPPM_GETNPPSETTINGSDIRPATH: lstrcpynW((wchar_t*)l, g_cfg.c_str(), (int)w); return (LRESULT)g_cfg.size();
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK SciProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case SCI_GETLENGTH: return (LRESULT)g_text.size();
        case SCI_GETCODEPAGE: return SC_CP_UTF8;
        case SCI_GETTEXT: {
            size_t n = (size_t)w ? (size_t)w - 1 : 0;
            if (n > g_text.size()) n = g_text.size();
            memcpy((char*)l, g_text.data(), n);
            ((char*)l)[n] = 0;
            return (LRESULT)n;
        }
        case SCI_GOTOLINE: g_gotoLine = (int)w; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// closes message boxes and prints their text
static DWORD WINAPI Watcher(LPVOID) {
    for (;;) {
        Sleep(150);
        HWND dlg = FindWindowW(L"#32770", nullptr);
        if (!dlg) continue;
        std::wstring all;
        HWND c = nullptr;
        while ((c = FindWindowExW(dlg, c, L"Static", nullptr))) {
            wchar_t buf[2048];
            GetWindowTextW(c, buf, 2048);
            if (*buf) { all += buf; all += L" | "; }
        }
        wprintf(L"  [MessageBox] %ls\n", all.c_str());
        fflush(stdout);
        ++g_dialogs;
        PostMessageW(dlg, WM_COMMAND, IDOK, 0);
        Sleep(300);
    }
}

static std::string readAll(const std::wstring& p) {
    std::ifstream in(p.c_str(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { printf("usage\n"); return 2; }
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { printf("FAIL LoadLibrary %lu\n", GetLastError()); return 1; }
    auto setInfo = (void (*)(NppData))GetProcAddress(dll, "setInfo");
    auto getFuncs = (FuncItem * (*)(int*)) GetProcAddress(dll, "getFuncsArray");
    auto getName = (const wchar_t* (*)())GetProcAddress(dll, "getName");
    auto isUni = (BOOL(*)())GetProcAddress(dll, "isUnicode");
    auto beNot = GetProcAddress(dll, "beNotified");
    auto msgProc = GetProcAddress(dll, "messageProc");
    if (!setInfo || !getFuncs || !getName || !isUni || !beNot || !msgProc) { printf("FAIL exports\n"); return 1; }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = NppProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FakeNpp";
    RegisterClassW(&wc);
    wc.lpfnWndProc = SciProc; wc.lpszClassName = L"FakeSci";
    RegisterClassW(&wc);
    NppData d;
    d._nppHandle = CreateWindowW(L"FakeNpp", L"npp", 0, 0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
    d._scintillaMainHandle = CreateWindowW(L"FakeSci", L"sci", 0, 0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
    d._scintillaSecondHandle = d._scintillaMainHandle;
    CreateThread(nullptr, 0, Watcher, nullptr, 0, nullptr);

    setInfo(d);
    int n = 0;
    FuncItem* f = getFuncs(&n);
    wprintf(L"plugin: %ls  isUnicode=%d  items=%d\n", getName(), isUni(), n);
    for (int i = 0; i < n; ++i)
        wprintf(L"  menu[%d] '%ls' %ls%ls\n", i, f[i]._itemName, f[i]._pFunc ? L"" : L"(separator)",
                f[i]._pShKey ? (f[i]._pShKey->_isCtrl && f[i]._pShKey->_isAlt ? L" Ctrl+Alt+Y" : L" key") : L"");

    g_path = argv[2];
    g_cfg = argv[3];
    g_text = readAll(argc > 4 ? argv[4] : argv[2]);

    // 1) render valid module
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring stem = g_path.substr(g_path.find_last_of(L"\\/") + 1);
    stem = stem.substr(0, stem.rfind(L'.'));
    std::wstring out = std::wstring(tmp) + L"yang2html\\" + stem + L".html";
    DeleteFileW(out.c_str());
    f[0]._pFunc();
    std::string html = readAll(out);
    wprintf(L"TEST render: output %ls  bytes=%zu  has-data=%d\n", out.c_str(), html.size(),
            (int)(html.find("id=\"data\">{\"module\"") != std::string::npos));
    printf("RESULT_HTML_PATH=%ls\n", out.c_str());
    CHECK(html.find("id=\"data\">{\"module\"") != std::string::npos, "render produces tree page");
    CHECK(argc > 4 || html.find("\"local\":[\"acme-olt\",\"acme-olt-ont\"]") != std::string::npos, "submodule resolved from file folder");

    // 2) syntax error -> message box + caret jump
    g_text = "module bad {\n  container x {\n    leaf y { type string;\n}\n";
    f[0]._pFunc();
    printf("TEST syntax error: gotoLine(0-based)=%d dialogs=%d\n", g_gotoLine, g_dialogs);
    CHECK(g_gotoLine == 1, "syntax error jumps to line 2");

    // 3) install syntax highlighting into <settings>\userDefineLangs\YANG.xml
    f[2]._pFunc();
    std::wstring udl = g_cfg + L"\\userDefineLangs\\YANG.xml";
    std::string udlText = readAll(udl);
    printf("TEST syntax install: bytes=%zu has-UserLang=%d\n", udlText.size(),
           (int)(udlText.find("<UserLang name=\"YANG\"") != std::string::npos));
    CHECK(udlText.find("<UserLang name=\"YANG\"") != std::string::npos, "syntax highlighting installed");

    // 4) search paths menu creates ini and asks Npp to open it
    f[3]._pFunc();
    wprintf(L"TEST search paths: DOOPEN=%ls exists=%d\n", g_opened.c_str(),
            GetFileAttributesW(g_opened.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(!g_opened.empty() && GetFileAttributesW(g_opened.c_str()) != INVALID_FILE_ATTRIBUTES, "search-paths ini created and opened");

    // 5) about
    f[4]._pFunc();
    printf("TEST about: dialogs=%d\n", g_dialogs);
    CHECK(g_dialogs >= 3, "message boxes shown (error, install, about)");
    printf("%s (%d failure(s))\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}
