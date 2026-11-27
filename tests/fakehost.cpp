// Fake Notepad++ host: loads YangTreeView.dll and drives it through the real
// plugin ABI with fake Notepad++ / Scintilla windows. Run under Wine or Windows.
//   fakehost.exe <dll> <yang file> <config dir> [<yang text file to use as buffer>]
// Exit code 0 = all checks passed.
// YANGTREEVIEW_HEADLESS=1          dialogs are logged instead of shown (CI)
// YANGTREEVIEW_SELFTEST_WEBVIEW=1  (with HEADLESS) also start the real WebView2 in the docked
//                                  panel, shown in a frame window, and wait for the page to load
// YANGTREEVIEW_SELFTEST_HOLD=<ms>  keep that window open a while (to look at / screenshot it)
// YANGTREEVIEW_SELFTEST_START_TIMEOUT=<ms>  (plugin) WebView2 start-up timeout, to test the
//                                  browser fallback (the webview checks then fail on purpose)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../src/npp_plugin_api.h"

static std::string g_text;
static std::wstring g_path, g_cfg, g_opened;
static int g_gotoLine = -1;
static int g_dialogs = 0;
static int g_fail = 0;
// docking
static HWND g_frame = nullptr;
static HWND g_dockClient = nullptr;
static std::wstring g_dockName, g_dockModule, g_dockInfo;
static int g_dockId = -1, g_dockRegs = 0;
static bool g_dockShown = false;
static bool g_dark = false;
static std::map<int, bool> g_checks;  // NPPM_SETMENUITEMCHECK by cmdID
static std::wstring g_savedPath;      // NPPM_GETFULLPATHFROMBUFFERID answer
static int g_newDocs = 0, g_lang = -1;  // "Open in new tab": File > New, language
static std::string g_setText;         // SCI_SETTEXT
#define CHECK(cond, what) do { if (cond) printf("  PASS %s\n", what); else { printf("  FAIL %s\n", what); ++g_fail; } fflush(stdout); } while (0)

static LRESULT CALLBACK NppProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case NPPM_GETCURRENTSCINTILLA: *(int*)l = 0; return TRUE;
        case NPPM_GETFULLCURRENTPATH: lstrcpynW((wchar_t*)l, g_path.c_str(), (int)w); return TRUE;
        case NPPM_GETPLUGINSCONFIGDIR: lstrcpynW((wchar_t*)l, g_cfg.c_str(), (int)w); return TRUE;
        case NPPM_DOOPEN: g_opened = (const wchar_t*)l; return TRUE;
        case NPPM_ISDARKMODEENABLED: return g_dark;
        case NPPM_GETSHORTCUTBYCMDID: {  // Ctrl+Alt+Y, as Notepad++ reports a mapped shortcut
            ShortcutKey* sk = (ShortcutKey*)l;
            sk->_isCtrl = true; sk->_isAlt = true; sk->_isShift = false; sk->_key = 'Y';
            return TRUE;
        }
        case NPPM_GETNPPSETTINGSDIRPATH: lstrcpynW((wchar_t*)l, g_cfg.c_str(), (int)w); return (LRESULT)g_cfg.size();
        case NPPM_DMMREGASDCKDLG: {
            const tTbData* tb = (const tTbData*)l;
            ++g_dockRegs;
            g_dockClient = tb->hClient;
            g_dockName = tb->pszName ? tb->pszName : L"";
            g_dockModule = tb->pszModuleName ? tb->pszModuleName : L"";
            g_dockInfo = tb->pszAddInfo ? tb->pszAddInfo : L"";
            g_dockId = tb->dlgID;
            // like Notepad++'s docking container: reparent as a child that fills the frame
            if (g_frame && IsWindow(tb->hClient)) {
                SetWindowLongPtrW(tb->hClient, GWL_STYLE, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
                SetParent(tb->hClient, g_frame);
                RECT rc;
                GetClientRect(g_frame, &rc);
                MoveWindow(tb->hClient, 0, 0, rc.right, rc.bottom, TRUE);
            }
            return TRUE;
        }
        case NPPM_DMMUPDATEDISPINFO: return TRUE;
        case NPPM_SETMENUITEMCHECK: g_checks[(int)w] = l != 0; return TRUE;
        case NPPM_MENUCOMMAND: if (l == IDM_FILE_NEW) ++g_newDocs; return TRUE;
        case NPPM_SETCURRENTLANGTYPE: g_lang = (int)l; return TRUE;
        case NPPM_GETFULLPATHFROMBUFFERID:
            if (l) lstrcpynW((wchar_t*)l, g_savedPath.c_str(), MAX_PATH);
            return (LRESULT)g_savedPath.size();
        case NPPM_DMMSHOW: g_dockShown = true; ShowWindow((HWND)l, SW_SHOW); return TRUE;
        case NPPM_DMMHIDE: g_dockShown = false; ShowWindow((HWND)l, SW_HIDE); return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK FrameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE && g_dockClient && IsWindow(g_dockClient)) MoveWindow(g_dockClient, 0, 0, LOWORD(l), HIWORD(l), TRUE);
    return DefWindowProcW(h, m, w, l);
}

// Runs the message loop until `pred` holds or `ms` elapse.
template <class F>
static bool pumpUntil(F pred, DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pred()) return true;
        if (GetTickCount64() >= end) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
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
        case SCI_SETTEXT: g_setText = (const char*)l; return 0;
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
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Headless mode (CI): plugin logs dialogs to a file instead of showing them.
    // Interactive mode (default): real message boxes, closed by the watcher thread.
    std::wstring dlgLog;
    bool headless = GetEnvironmentVariableW(L"YANGTREEVIEW_HEADLESS", nullptr, 0) > 0;
    if (headless) {
        dlgLog = std::wstring(argv[3]) + L"\\dialogs.log";
        DeleteFileW(dlgLog.c_str());
        SetEnvironmentVariableW(L"YANGTREEVIEW_SELFTEST", dlgLog.c_str());
        printf("headless mode: dialogs -> %ls\n", dlgLog.c_str());
    }
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { printf("FAIL LoadLibrary %lu\n", GetLastError()); return 1; }
    auto setInfo = (void (*)(NppData))GetProcAddress(dll, "setInfo");
    auto getFuncs = (FuncItem * (*)(int*)) GetProcAddress(dll, "getFuncsArray");
    auto getName = (const wchar_t* (*)())GetProcAddress(dll, "getName");
    auto isUni = (BOOL(*)())GetProcAddress(dll, "isUnicode");
    auto beNot = GetProcAddress(dll, "beNotified");
    auto msgProc = GetProcAddress(dll, "messageProc");
    if (!setInfo || !getFuncs || !getName || !isUni || !beNot || !msgProc) { printf("FAIL exports\n"); return 1; }
    auto notify = (void (*)(SCNotification*))(void*)beNot;
    bool webview = headless && GetEnvironmentVariableW(L"YANGTREEVIEW_SELFTEST_WEBVIEW", nullptr, 0) > 0;
    if (webview)  // NETCONF builder, end to end: select mtu, edit-config, value 9000, "Open in new tab"
        SetEnvironmentVariableW(L"YANGTREEVIEW_SELFTEST_SCRIPT",
            L"(function(){var row=[].slice.call(document.querySelectorAll('#tree .row')).filter(function(r){"
            L"var n=r.querySelector('.nm');return n&&n.textContent==='mtu';})[0];row.click();"
            L"document.querySelector('.nc-ops button[data-op=\"edit-config\"]').click();"
            L"var i=document.querySelector('#ncV input[type=text]');i.value='9000';"
            L"i.dispatchEvent(new Event('input',{bubbles:true}));document.getElementById('ncNew').click();"
            L"return document.getElementById('ncOut').textContent.length;})()");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = NppProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FakeNpp";
    RegisterClassW(&wc);
    wc.lpfnWndProc = SciProc; wc.lpszClassName = L"FakeSci";
    RegisterClassW(&wc);
    NppData d;
    d._nppHandle = CreateWindowW(L"FakeNpp", L"npp", 0, 0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
    d._scintillaMainHandle = CreateWindowW(L"FakeSci", L"sci", 0, 0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
    d._scintillaSecondHandle = d._scintillaMainHandle;
    if (!headless) CreateThread(nullptr, 0, Watcher, nullptr, 0, nullptr);
    // the docking container stand-in (off-screen unless the WebView is shown): the panel
    // must be visible for the automatic refresh
    wc.lpfnWndProc = FrameProc; wc.lpszClassName = L"FakeDock";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_frame = CreateWindowW(L"FakeDock", L"fakehost - docked YANG Tree View", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            webview ? 40 : -4000, webview ? 40 : -4000, 1100, 760, nullptr, nullptr, nullptr, nullptr);

    g_cfg = argv[3];  // settings are read in setInfo
    setInfo(d);
    int n = 0;
    FuncItem* f = getFuncs(&n);
    for (int i = 0; i < n; ++i) f[i]._cmdID = 22000 + i;  // as Notepad++ assigns them
    auto itemIdx = [&](const wchar_t* name) {
        for (int i = 0; i < n; ++i)
            if (lstrcmpW(f[i]._itemName, name) == 0) return i;
        return -1;
    };
    auto item = [&](const wchar_t* name) -> PFUNCPLUGINCMD {
        for (int i = 0; i < n; ++i)
            if (lstrcmpW(f[i]._itemName, name) == 0 && f[i]._pFunc) return f[i]._pFunc;
        wprintf(L"  FAIL menu item '%ls' missing\n", name);
        ++g_fail;
        return nullptr;
    };
    auto run = [&](const wchar_t* name) { if (PFUNCPLUGINCMD fn = item(name)) fn(); };
    auto send = [&](UINT code) {
        SCNotification sn = {};
        sn.nmhdr.hwndFrom = d._nppHandle;
        sn.nmhdr.code = code;
        notify(&sn);
    };
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

    // 0) panel restored at startup: Notepad++ runs the command before NPPN_READY, when the
    //    files aren't loaded yet -> placeholder, no error box / caret jump; the active tab
    //    is rendered into the panel on NPPN_READY
    {
        std::string saved = g_text;
        g_text = "just some notes, not YANG";
        std::wstring ph = std::wstring(tmp) + L"yang2html\\_yangtreeview_start.html";
        DeleteFileW(ph.c_str());
        DeleteFileW(out.c_str());
        int dialogsBefore = g_dialogs;
        run(L"Show YANG tree");
        std::string page = readAll(ph);
        printf("TEST restore at startup: placeholder bytes=%zu gotoLine=%d registered=%d shown=%d\n", page.size(),
               g_gotoLine, g_dockRegs, (int)g_dockShown);
        CHECK(page.find("Ctrl+Alt+Y") != std::string::npos, "restore: placeholder page written");
        CHECK(g_gotoLine == -1 && g_dialogs == dialogsBefore, "restore: no error box or caret jump");
        CHECK(g_dockRegs == 1 && g_dockShown, "restore: panel registered and shown");
        g_text = saved;
        send(NPPN_READY);
        CHECK(readAll(out).find("id=\"data\">{\"module\"") != std::string::npos, "restore: tree rendered on NPPN_READY");
    }

    DeleteFileW(out.c_str());
    run(L"Show YANG tree");
    std::string html = readAll(out);
    wprintf(L"TEST render: output %ls  bytes=%zu  has-data=%d\n", out.c_str(), html.size(),
            (int)(html.find("id=\"data\">{\"module\"") != std::string::npos));
    printf("RESULT_HTML_PATH=%ls\n", out.c_str());
    CHECK(html.find("id=\"data\">{\"module\"") != std::string::npos, "render produces tree page");
    CHECK(argc > 4 || html.find("\"local\":[\"acme-olt\",\"acme-olt-ont\"]") != std::string::npos, "submodule resolved from file folder");

    // 1b) docked panel: registered once with Notepad++, shown, caption = module name
    wprintf(L"TEST dock: regs=%d client=%p name='%ls' module='%ls' info='%ls' dlgID=%d shown=%d\n", g_dockRegs,
            (void*)g_dockClient, g_dockName.c_str(), g_dockModule.c_str(), g_dockInfo.c_str(), g_dockId, (int)g_dockShown);
    CHECK(g_dockRegs == 1 && IsWindow(g_dockClient), "dock: panel window registered once");
    CHECK(g_dockName == L"YANG Tree View" && g_dockId == 0, "dock: name and dlgID (= Show YANG tree)");
    CHECK(g_dockModule.size() > 4 && _wcsicmp(g_dockModule.c_str() + g_dockModule.size() - 4, L".dll") == 0,
          "dock: pszModuleName is the DLL file name");
    CHECK(g_dockShown, "dock: panel shown");

    CHECK(html.find("\"paths\":{") != std::string::npos, "plugin page embeds source paths (jump to source)");

    // 1e) automatic refresh while the panel is visible (headless: read from the log)
    if (headless) {
        auto shownCount = [&]() {
            std::string log = readAll(dlgLog);
            size_t n = 0;
            for (size_t p = log.find("[Dock] shown"); p != std::string::npos; p = log.find("[Dock] shown", p + 1)) ++n;
            for (size_t p = log.find("[Dock] navigate"); p != std::string::npos; p = log.find("[Dock] navigate", p + 1)) ++n;
            return n;
        };
        size_t n0 = shownCount();
        send(NPPN_BUFFERACTIVATED);  // same module, unchanged
        CHECK(shownCount() == n0, "auto refresh: unchanged module is not reloaded");
        std::string saved = g_text;
        const std::string needle = "description \"Initial.\"";
        size_t at = g_text.find(needle);
        if (at != std::string::npos) g_text.replace(at, needle.size(), "description \"Initial, edited.\"");
        send(NPPN_FILESAVED);
        CHECK(at != std::string::npos && shownCount() == n0 + 1, "auto refresh: saved edit refreshes the panel");
        int gl = g_gotoLine, dl = g_dialogs;
        g_text = "module acme-olt {\n  container x {\n";
        send(NPPN_FILESAVED);
        std::string log = readAll(dlgLog);
        CHECK(shownCount() == n0 + 1 && log.find("[Dock] status: Not updated: line") != std::string::npos,
              "auto refresh: syntax error keeps the tree and reports it in the panel");
        CHECK(g_gotoLine == gl && g_dialogs == dl && log.find("[MessageBox] Could not parse") == std::string::npos,
              "auto refresh: no message box or caret jump");
        g_text = saved;
        std::wstring savedPath = g_path;
        g_path = g_cfg + L"\\notes.txt";
        send(NPPN_BUFFERACTIVATED);
        CHECK(shownCount() == n0 + 1, "auto refresh: non-YANG tab is ignored");
        g_path = savedPath;
        send(NPPN_BUFFERACTIVATED);  // back to the module: refreshed (text differs from the edit)
        CHECK(shownCount() == n0 + 2, "auto refresh: switching back re-renders the module");
    }

    // 1c) real WebView2 in the panel (opt-in; needs the WebView2 Runtime and a desktop)
    if (webview) {
        auto loaded = [&]() {
            std::string log = readAll(dlgLog);
            return log.find("navigation completed: ok") != std::string::npos ||
                   log.find("WebView2 failed") != std::string::npos;
        };
        bool done = pumpUntil(loaded, 30000);
        std::string log = readAll(dlgLog);
        printf("TEST webview: finished=%d\n", (int)done);
        CHECK(log.find("navigation completed: ok") != std::string::npos, "webview: page loaded in docked panel");
        CHECK(log.find("WebView2 failed") == std::string::npos, "webview: no WebView2 failure");
        // the test script filled in the NETCONF form and pressed "Open in new tab"
        pumpUntil([] { return g_newDocs > 0; }, 10000);
        printf("TEST netconf new tab: newdocs=%d lang=%d\n%s\n", g_newDocs, g_lang, g_setText.c_str());
        CHECK(g_newDocs == 1 && g_lang == L_XML, "netconf: new tab opened as XML");
        CHECK(g_setText.find("<mtu nc:operation=\"merge\">9000</mtu>") != std::string::npos &&
                  g_setText.find("<target><running/></target>") != std::string::npos,
              "netconf: edit-config request for mtu in the new tab");
        CHECK(g_setText.find("\r\n") != std::string::npos, "netconf: line endings follow the document (CRLF)");
        // Notepad++ switches to dark mode; the panel becomes a narrow side panel
        g_dark = true;
        send(NPPN_DARKMODECHANGED);
        RECT fr;
        GetWindowRect(g_frame, &fr);
        MoveWindow(g_frame, fr.left, fr.top, 480, fr.bottom - fr.top, TRUE);
        pumpUntil([] { return false; }, 500);
        CHECK(readAll(dlgLog).find("[Dock] theme dark") != std::string::npos, "webview: dark mode change sent to panel");
        wchar_t hold[32] = {0};
        if (GetEnvironmentVariableW(L"YANGTREEVIEW_SELFTEST_HOLD", hold, 32)) pumpUntil([] { return false; }, (DWORD)_wtoi(hold));
        if (g_newDocs)  // someone used "Open in new tab" in the window during the hold
            printf("NEWDOC count=%d lang=%d text:\n%s\nENDDOC\n", g_newDocs, g_lang, g_setText.c_str());
    }

    // 1d) browser mode still works
    DeleteFileW(out.c_str());
    run(L"Show YANG tree in browser");
    CHECK(readAll(out).find("id=\"data\">{\"module\"") != std::string::npos, "browser: page written");

    // 2) syntax error -> message box + caret jump
    g_text = "module bad {\n  container x {\n    leaf y { type string;\n}\n";
    run(L"Show YANG tree");
    printf("TEST syntax error: gotoLine(0-based)=%d dialogs=%d\n", g_gotoLine, g_dialogs);
    CHECK(g_gotoLine == 1, "syntax error jumps to line 2");

    // 3) install syntax highlighting into <settings>\userDefineLangs\YANG.xml
    run(L"Install YANG syntax highlighting");
    std::wstring udl = g_cfg + L"\\userDefineLangs\\YANG.xml";
    std::string udlText = readAll(udl);
    printf("TEST syntax install: bytes=%zu has-UserLang=%d\n", udlText.size(),
           (int)(udlText.find("<UserLang name=\"YANG\"") != std::string::npos));
    CHECK(udlText.find("<UserLang name=\"YANG\"") != std::string::npos, "syntax highlighting installed");

    // 4) search paths menu creates ini and asks Npp to open it
    run(L"Search paths and settings...");
    wprintf(L"TEST search paths: DOOPEN=%ls exists=%d\n", g_opened.c_str(),
            GetFileAttributesW(g_opened.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(!g_opened.empty() && GetFileAttributesW(g_opened.c_str()) != INVALID_FILE_ATTRIBUTES, "search-paths ini created and opened");

    // 4b) settings: menu toggles write YangTreeView.ini; saving the ini applies it
    {
        g_text = readAll(argc > 4 ? argv[4] : argv[2]);  // test 2 left a broken module in the buffer
        std::wstring ini = g_cfg + L"\\YangTreeView.ini";
        int dockIdx = itemIdx(L"Use docked panel"), followIdx = itemIdx(L"Panel follows active tab");
        CHECK(dockIdx > 0 && followIdx > 0 && f[dockIdx]._init2Check && f[followIdx]._init2Check,
              "settings: menu items, checked by default");
        run(L"Use docked panel");
        CHECK(readAll(ini).find("panel=browser") != std::string::npos && !g_checks[f[dockIdx]._cmdID] && !g_dockShown,
              "settings: 'Use docked panel' off -> ini panel=browser, unchecked, panel hidden");
        if (headless) {
            size_t before = readAll(dlgLog).size();
            run(L"Show YANG tree");
            std::string tail = readAll(dlgLog).substr(before);
            CHECK(tail.find("[Browser]") != std::string::npos && tail.find("[Dock] shown") == std::string::npos &&
                      tail.find("[Dock] navigate") == std::string::npos,
                  "settings: panel=browser -> Show YANG tree opens the browser");
        }
        run(L"Use docked panel");
        CHECK(readAll(ini).find("panel=docked") != std::string::npos && g_checks[f[dockIdx]._cmdID],
              "settings: 'Use docked panel' back on");
        run(L"Show YANG tree");
        run(L"Panel follows active tab");
        CHECK(readAll(ini).find("follow=no") != std::string::npos && !g_checks[f[followIdx]._cmdID],
              "settings: 'Panel follows active tab' off -> follow=no");
        if (headless) {
            size_t before = readAll(dlgLog).size();
            std::string saved = g_text;
            g_text += "\n";
            size_t at = g_text.find("description \"Initial");
            if (at != std::string::npos) g_text.insert(at, "contact \"x\"; ");
            send(NPPN_FILESAVED);
            send(NPPN_BUFFERACTIVATED);
            std::string tail = readAll(dlgLog).substr(before);
            CHECK(tail.find("[Dock]") == std::string::npos, "settings: follow=no -> no automatic refresh");
            g_text = saved;
        }
        // hand-edited ini, saved in Notepad++
        {
            std::string t = readAll(ini);
            // (rfind: the template's comment lines mention the same keys)
            t.replace(t.rfind("follow=no"), 9, "follow=yes");
            t.replace(t.rfind("panel=docked"), 12, "panel=browser");
            std::ofstream(ini.c_str(), std::ios::binary | std::ios::trunc) << t;
            g_savedPath = ini;
            SCNotification sn = {};
            sn.nmhdr.hwndFrom = d._nppHandle;
            sn.nmhdr.code = NPPN_FILESAVED;
            sn.nmhdr.idFrom = 42;
            notify(&sn);
            CHECK(g_checks[f[followIdx]._cmdID] && !g_checks[f[dockIdx]._cmdID],
                  "settings: saving the edited ini applies it");
            run(L"Use docked panel");  // back to the default
            g_savedPath.clear();
        }
        // search paths still parse around the settings lines (they must not become folders)
        std::string t = readAll(ini);
        CHECK(t.find("# C:\\yang") != std::string::npos && t.find("panel=docked") != std::string::npos &&
                  t.find("follow=yes") != std::string::npos,
              "settings: ini keeps comments and both keys");
    }

    // 5) about
    run(L"About");
    printf("TEST about: dialogs=%d\n", g_dialogs);
    if (headless) {
        std::string log = readAll(dlgLog);
        size_t n = 0;
        for (size_t p = log.find("[MessageBox]"); p != std::string::npos; p = log.find("[MessageBox]", p + 1)) ++n;
        printf("%s", log.c_str());
        g_dialogs = (int)n;
        CHECK(log.find("missing '}'") != std::string::npos, "syntax error reported with message");
    }
    CHECK(g_dialogs >= 3, "message boxes shown (error, install, about)");
    send(NPPN_SHUTDOWN);
    pumpUntil([] { return false; }, 300);
    printf("%s (%d failure(s))\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}
