// YangTreeView - docked viewer panel (WebView2 inside a Notepad++ docking container)
//
// Flow: show() creates a plain child window, registers it with NPPM_DMMREGASDCKDLG and
// shows it. The WebView2 environment/controller are created asynchronously on first use;
// the requested page is kept until the controller is ready. Pages are served from the
// render folder through a virtual host (https://yangtreeview.example/...) rather than
// NavigateToString, which is limited to 2 MB, and so the page runs in a secure origin
// (navigator.clipboard and localStorage behave as in the browser).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "dock_panel.hpp"

#include <objbase.h>  // COM "interface" macro (excluded by WIN32_LEAN_AND_MEAN), needed by WebView2.h
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cwchar>
#include <utility>

#include "WebView2.h"

#ifndef YTV_WEBVIEW2_STATIC
// MinGW builds: WebView2LoaderStatic.lib is MSVC-only, so WebView2Loader.dll is loaded
// at runtime from the plugin folder.
#define YTV_WEBVIEW2_DYNAMIC 1
#endif

namespace dock {

static const wchar_t kHostName[] = L"yangtreeview.example";
static const wchar_t kPanelName[] = L"YANG Tree View";
static const wchar_t kWndClass[] = L"YangTreeViewDockPanel";
static const UINT WM_YTV_DEFER = WM_APP + 0x59;  // lParam: std::function<void()>*

// ------------------------------------------------------------------------- //
// minimal COM helpers (no WRL, so MinGW can build this too)
// ------------------------------------------------------------------------- //

template <class T>
class Ref {
   public:
    Ref() = default;
    Ref(const Ref& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Ref& operator=(const Ref& o) { Ref(o).swap(*this); return *this; }
    ~Ref() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    void attach(T* p) { reset(); p_ = p; }
    void assign(T* p) { if (p) p->AddRef(); attach(p); }   // AddRef + keep
    T** put() { reset(); return &p_; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void swap(Ref& o) { std::swap(p_, o.p_); }
    template <class U>
    Ref<U> query(const IID& iid) const {
        Ref<U> r;
        if (p_) p_->QueryInterface(iid, reinterpret_cast<void**>(r.put()));
        return r;
    }

   private:
    T* p_ = nullptr;
};

// Deduce the two Invoke() argument types of a WebView2 handler interface.
template <class M>
struct InvokeSig;
template <class C, class A, class B>
struct InvokeSig<HRESULT (STDMETHODCALLTYPE C::*)(A, B)> {
    using First = A;
    using Second = B;
};

template <class I>
class Callback final : public I {
    using A = typename InvokeSig<decltype(&I::Invoke)>::First;
    using B = typename InvokeSig<decltype(&I::Invoke)>::Second;

   public:
    Callback(const IID& iid, std::function<HRESULT(A, B)> fn) : iid_(iid), fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A a, B b) override { return fn_ ? fn_(a, b) : S_OK; }

   private:
    IID iid_;
    std::function<HRESULT(A, B)> fn_;
    std::atomic<ULONG> refs_{1};
};

// Returns an owning reference (refcount 1); the API we pass it to adds its own.
template <class I, class F>
static Ref<I> callback(const IID& iid, F&& fn) {
    Ref<I> r;
    r.attach(new Callback<I>(iid, std::forward<F>(fn)));
    return r;
}

// ------------------------------------------------------------------------- //
// WebView2 loader (static lib with MSVC, WebView2Loader.dll otherwise)
// ------------------------------------------------------------------------- //

typedef HRESULT(STDAPICALLTYPE* PFN_CreateEnv)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                               ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
typedef HRESULT(STDAPICALLTYPE* PFN_GetVersion)(PCWSTR, LPWSTR*);

// ------------------------------------------------------------------------- //
// state (all on Notepad++'s UI thread)
// ------------------------------------------------------------------------- //

static Config g_cfg;
static HWND g_host = nullptr;
static bool g_registered = false;
static bool g_creating = false;
// Bumped whenever a creation attempt is abandoned, so late callbacks from it are ignored.
static unsigned g_gen = 0;
static const UINT_PTR kStartTimer = 1;
static const UINT kStartTimeoutMs = 15000;  // WebView2 normally starts in < 1 s
static UINT g_startTimeoutMs = kStartTimeoutMs;
static bool g_shuttingDown = false;
static Ref<ICoreWebView2Environment> g_env;
static Ref<ICoreWebView2Controller> g_ctl;
static Ref<ICoreWebView2> g_wv;
static std::wstring g_pendingUrl;
// Notepad++ keeps the pszAddInfo pointer, so the caption lives in a fixed buffer.
static wchar_t g_caption[256] = {0};
static HWND g_rootSubclassed = nullptr;
static tTbData g_tb = {};
static wchar_t g_moduleName[MAX_PATH] = {0};

static void log(const std::wstring& s) {
    if (g_cfg.log) g_cfg.log(L"[Dock] " + s);
}

static std::wstring hex(HRESULT hr) {
    wchar_t b[16];
    swprintf(b, 16, L"0x%08lX", (unsigned long)hr);
    return b;
}

static bool loader(PFN_CreateEnv* create, PFN_GetVersion* version) {
#ifdef YTV_WEBVIEW2_DYNAMIC
    static HMODULE h = nullptr;
    if (!h) {
        wchar_t self[MAX_PATH * 2] = {0};
        GetModuleFileNameW(g_cfg.module, self, (DWORD)(sizeof self / sizeof self[0]));
        fs::path p = fs::path(self).parent_path() / L"WebView2Loader.dll";
        h = LoadLibraryW(p.c_str());
        if (!h) h = LoadLibraryW(L"WebView2Loader.dll");
        if (!h) return false;
    }
    if (create) *create = (PFN_CreateEnv)(void*)GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
    if (version) *version = (PFN_GetVersion)(void*)GetProcAddress(h, "GetAvailableCoreWebView2BrowserVersionString");
    return (!create || *create) && (!version || *version);
#else
    if (create) *create = &CreateCoreWebView2EnvironmentWithOptions;
    if (version) *version = &GetAvailableCoreWebView2BrowserVersionString;
    return true;
#endif
}

std::wstring runtimeVersion() {
    PFN_GetVersion getVersion = nullptr;
    if (!loader(nullptr, &getVersion)) return L"";
    LPWSTR v = nullptr;
    std::wstring out;
    if (SUCCEEDED(getVersion(nullptr, &v)) && v) out = v;
    if (v) CoTaskMemFree(v);
    return out;
}

static bool darkMode() {
    return SendMessageW(g_cfg.npp._nppHandle, NPPM_ISDARKMODEENABLED, 0, 0) != 0;
}

// Background before the page paints (avoids a white flash in dark mode).
static COLORREF backgroundColor() { return darkMode() ? RGB(0x1e, 0x1f, 0x22) : RGB(0xff, 0xff, 0xff); }

static std::wstring pageUrl(const fs::path& page) {
    std::string u8;
    {
        std::wstring w = page.filename().wstring();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        u8.assign((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &u8[0], n, nullptr, nullptr);
    }
    std::wstring url = std::wstring(L"https://") + kHostName + L"/";
    static const char* hexd = "0123456789ABCDEF";
    for (unsigned char c : u8) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                     c == '.' || c == '_' || c == '~';
        if (plain) {
            url += (wchar_t)c;
        } else {
            url += L'%';
            url += (wchar_t)hexd[c >> 4];
            url += (wchar_t)hexd[c & 15];
        }
    }
    // same file is rewritten on every render: make each navigation unique
    url += L"?v=" + std::to_wstring(GetTickCount64());
    url += darkMode() ? L"&theme=dark" : L"&theme=light";  // initial theme, no flash
    return url;
}

static bool isOurUrl(const wchar_t* uri) {
    std::wstring prefix = std::wstring(L"https://") + kHostName + L"/";
    return uri && wcsncmp(uri, prefix.c_str(), prefix.size()) == 0;
}

static void openExternally(const wchar_t* uri) {
    if (uri && (wcsncmp(uri, L"https://", 8) == 0 || wcsncmp(uri, L"http://", 7) == 0))
        ShellExecuteW(g_cfg.npp._nppHandle, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

static void resize() {
    if (!g_ctl || !g_host) return;
    RECT rc;
    GetClientRect(g_host, &rc);
    g_ctl->put_Bounds(rc);
}

// The WebView must be told when an ancestor window moves, or popups such as the
// <select> dropdown open at the old position. The panel's top-level window changes
// when it is floated or docked, so the subclass follows it.
static LRESULT CALLBACK RootSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_WINDOWPOSCHANGED || m == WM_MOVE) {
        if (g_ctl) g_ctl->NotifyParentWindowPositionChanged();
    } else if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, RootSubclassProc, id);
        if (g_rootSubclassed == h) g_rootSubclassed = nullptr;
    }
    return DefSubclassProc(h, m, w, l);
}

static void trackRoot() {
    if (!g_host) return;
    HWND root = GetAncestor(g_host, GA_ROOT);
    if (root == g_host) root = nullptr;  // not reparented yet
    if (root == g_rootSubclassed) return;
    if (g_rootSubclassed) RemoveWindowSubclass(g_rootSubclassed, RootSubclassProc, 1);
    g_rootSubclassed = nullptr;
    if (root && SetWindowSubclass(root, RootSubclassProc, 1, 0)) g_rootSubclassed = root;
    if (g_ctl) g_ctl->NotifyParentWindowPositionChanged();
}

static void releaseWebView() {
    ++g_gen;
    if (g_host) KillTimer(g_host, kStartTimer);
    if (g_ctl) g_ctl->Close();
    g_wv.reset();
    g_ctl.reset();
    g_env.reset();
    g_creating = false;
}

static void fail(const std::wstring& why) {
    log(L"WebView2 failed: " + why);
    releaseWebView();
    g_pendingUrl.clear();
    if (g_registered) hide();
    if (g_cfg.onFailure) g_cfg.onFailure(why);
}

static void navigate(const std::wstring& url) {
    if (g_wv) {
        HRESULT hr = g_wv->Navigate(url.c_str());
        log(L"navigate " + url + L" -> " + hex(hr));
    } else {
        g_pendingUrl = url;  // WebView2 still starting: the latest request wins
        log(L"navigate (queued) " + url);
    }
}

static HRESULT onController(unsigned gen, HRESULT result, ICoreWebView2Controller* ctl) {
    if (gen != g_gen || g_shuttingDown) {  // abandoned (timed out, failed or shutting down)
        if (ctl) ctl->Close();
        return S_OK;
    }
    g_creating = false;
    if (g_host) KillTimer(g_host, kStartTimer);
    if (FAILED(result) || !ctl) {
        fail(L"CreateCoreWebView2Controller " + hex(result));
        return S_OK;
    }
    g_ctl.assign(ctl);
    if (FAILED(g_ctl->get_CoreWebView2(g_wv.put())) || !g_wv) {
        fail(L"get_CoreWebView2");
        return S_OK;
    }

    if (auto c2 = g_ctl.query<ICoreWebView2Controller2>(IID_ICoreWebView2Controller2)) {
        COLORREF c = backgroundColor();
        COREWEBVIEW2_COLOR bg = {255, GetRValue(c), GetGValue(c), GetBValue(c)};
        c2->put_DefaultBackgroundColor(bg);
    }

    Ref<ICoreWebView2Settings> st;
    if (SUCCEEDED(g_wv->get_Settings(st.put())) && st) {
        st->put_IsStatusBarEnabled(FALSE);
        st->put_AreDevToolsEnabled(GetEnvironmentVariableW(L"YANGTREEVIEW_DEVTOOLS", nullptr, 0) > 0);
        st->put_AreHostObjectsAllowed(FALSE);
        st->put_IsWebMessageEnabled(TRUE);  // page <-> plugin messages (jump to source, refresh)
        st->put_IsZoomControlEnabled(TRUE);
    }

    std::wstring dir = g_cfg.pagesDir.wstring();
    if (auto wv3 = g_wv.query<ICoreWebView2_3>(IID_ICoreWebView2_3)) {
        HRESULT hr = wv3->SetVirtualHostNameToFolderMapping(kHostName, dir.c_str(),
                                                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        if (FAILED(hr)) { fail(L"SetVirtualHostNameToFolderMapping " + hex(hr)); return S_OK; }
    } else {
        fail(L"WebView2 Runtime too old (ICoreWebView2_3 missing)");
        return S_OK;
    }

    EventRegistrationToken tok;
    // keep the panel on our pages; anything else opens in the default browser
    g_wv->add_NavigationStarting(
        callback<ICoreWebView2NavigationStartingEventHandler>(
            IID_ICoreWebView2NavigationStartingEventHandler,
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                LPWSTR uri = nullptr;
                a->get_Uri(&uri);
                if (!isOurUrl(uri)) {
                    a->put_Cancel(TRUE);
                    openExternally(uri);
                }
                if (uri) CoTaskMemFree(uri);
                return S_OK;
            })
            .get(),
        &tok);
    g_wv->add_NewWindowRequested(
        callback<ICoreWebView2NewWindowRequestedEventHandler>(
            IID_ICoreWebView2NewWindowRequestedEventHandler,
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                LPWSTR uri = nullptr;
                a->get_Uri(&uri);
                a->put_Handled(TRUE);
                openExternally(uri);
                if (uri) CoTaskMemFree(uri);
                return S_OK;
            })
            .get(),
        &tok);
    g_wv->add_NavigationCompleted(
        callback<ICoreWebView2NavigationCompletedEventHandler>(
            IID_ICoreWebView2NavigationCompletedEventHandler,
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                BOOL ok = FALSE;
                a->get_IsSuccess(&ok);
                COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                a->get_WebErrorStatus(&st);
                log(std::wstring(L"navigation completed: ") + (ok ? L"ok" : L"error " + std::to_wstring((int)st)));
                // self-test only: drive the page once (YANGTREEVIEW_SELFTEST_SCRIPT, with the
                // self-test log set), e.g. fill in the NETCONF form and press its buttons
                static bool ran = false;
                wchar_t script[4096] = {0};
                if (ok && !ran && g_wv && GetEnvironmentVariableW(L"YANGTREEVIEW_SELFTEST", nullptr, 0) > 0 &&
                    GetEnvironmentVariableW(L"YANGTREEVIEW_SELFTEST_SCRIPT", script, 4096) > 0) {
                    ran = true;
                    HRESULT hr = g_wv->ExecuteScript(
                        script, callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                    IID_ICoreWebView2ExecuteScriptCompletedHandler,
                                    [](HRESULT r, LPCWSTR json) -> HRESULT {
                                        log(L"test script: " + hex(r) + L" " + (json ? json : L""));
                                        return S_OK;
                                    })
                                    .get());
                    log(L"test script started " + hex(hr));
                }
                return S_OK;
            })
            .get(),
        &tok);
    // Browser process gone (crash, runtime update): start over on the next show().
    g_wv->add_ProcessFailed(
        callback<ICoreWebView2ProcessFailedEventHandler>(
            IID_ICoreWebView2ProcessFailedEventHandler,
            [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND k = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                a->get_ProcessFailedKind(&k);
                log(L"process failed, kind " + std::to_wstring((int)k));
                if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                    // the controller is unusable: close it (removes its window) and
                    // recreate it on the next show()
                    releaseWebView();
                } else if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED && g_wv) {
                    g_wv->Reload();
                }
                return S_OK;
            })
            .get(),
        &tok);

    // page -> plugin: plain string commands
    g_wv->add_WebMessageReceived(
        callback<ICoreWebView2WebMessageReceivedEventHandler>(
            IID_ICoreWebView2WebMessageReceivedEventHandler,
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                LPWSTR src = nullptr;
                a->get_Source(&src);
                bool ours = isOurUrl(src);
                if (src) CoTaskMemFree(src);
                LPWSTR m = nullptr;
                if (ours && SUCCEEDED(a->TryGetWebMessageAsString(&m)) && m) {
                    std::wstring msg = m;
                    log(L"message: " + msg);
                    if (g_cfg.onMessage) defer([msg] { g_cfg.onMessage(msg); });
                }
                if (m) CoTaskMemFree(m);
                return S_OK;
            })
            .get(),
        &tok);
    // keys pressed inside the panel never reach Notepad++'s accelerator table
    g_ctl->add_AcceleratorKeyPressed(
        callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            IID_ICoreWebView2AcceleratorKeyPressedEventHandler,
            [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                a->get_KeyEventKind(&kind);
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                    return S_OK;
                COREWEBVIEW2_PHYSICAL_KEY_STATUS st;
                if (SUCCEEDED(a->get_PhysicalKeyStatus(&st)) && st.WasKeyDown) return S_OK;  // auto-repeat
                UINT vk = 0;
                a->get_VirtualKey(&vk);
                bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0,
                     shift = GetKeyState(VK_SHIFT) < 0;
                if (g_cfg.onAccelerator && g_cfg.onAccelerator(vk, ctrl, alt, shift)) a->put_Handled(TRUE);
                return S_OK;
            })
            .get(),
        &tok);

    resize();
    trackRoot();
    g_ctl->put_IsVisible(IsWindowVisible(g_host));
    log(L"WebView2 ready");
    std::wstring url;
    url.swap(g_pendingUrl);
    if (!url.empty()) navigate(url);
    return S_OK;
}

static HRESULT onEnvironment(unsigned gen, HRESULT result, ICoreWebView2Environment* env) {
    if (gen != g_gen || g_shuttingDown) return S_OK;  // abandoned
    if (FAILED(result) || !env) {
        fail(L"CreateCoreWebView2Environment " + hex(result));
        return S_OK;
    }
    g_env.assign(env);
    HRESULT hr = env->CreateCoreWebView2Controller(
        g_host, callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                    [gen](HRESULT r, ICoreWebView2Controller* c) { return onController(gen, r, c); })
                    .get());
    if (FAILED(hr)) fail(L"CreateCoreWebView2Controller " + hex(hr));
    return S_OK;
}

static void createWebView() {
    if (g_creating || g_ctl) return;
    PFN_CreateEnv create = nullptr;
    if (!loader(&create, nullptr)) {
        fail(L"WebView2Loader.dll not found next to the plugin");
        return;
    }
    // WebView2 needs a single-threaded apartment; Notepad++'s UI thread already is one
    // (OLE drag and drop), in which case this just adds a reference.
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (co == RPC_E_CHANGED_MODE) log(L"warning: UI thread is not STA");

    std::error_code ec;
    fs::create_directories(g_cfg.userDataDir, ec);
    g_creating = true;
    unsigned gen = ++g_gen;
    log(L"creating WebView2, profile " + g_cfg.userDataDir.wstring());
    // if neither callback arrives (seen once while a previous browser process was exiting),
    // give up and fall back to the browser instead of leaving the panel blank
    g_startTimeoutMs = kStartTimeoutMs;
    if (const wchar_t* t = _wgetenv(L"YANGTREEVIEW_SELFTEST_START_TIMEOUT")) g_startTimeoutMs = std::max(1, _wtoi(t));  // test hook
    if (g_host) SetTimer(g_host, kStartTimer, g_startTimeoutMs, nullptr);
    HRESULT hr = create(nullptr, g_cfg.userDataDir.c_str(), nullptr,
                        callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                            [gen](HRESULT r, ICoreWebView2Environment* e) { return onEnvironment(gen, r, e); })
                            .get());
    if (FAILED(hr)) fail(L"CreateCoreWebView2EnvironmentWithOptions " + hex(hr));
}

// ------------------------------------------------------------------------- //
// panel window
// ------------------------------------------------------------------------- //

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_YTV_DEFER: {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(l));
            if (fn && *fn) (*fn)();
            return 0;
        }
        case WM_TIMER:
            if (w == kStartTimer) {
                KillTimer(h, kStartTimer);
                if (g_creating) fail(L"WebView2 did not start within " + std::to_wstring(g_startTimeoutMs) + L" ms");
                return 0;
            }
            break;
        case WM_SIZE:
            resize();
            return 0;
        case WM_WINDOWPOSCHANGED:
            trackRoot();
            if (g_ctl) g_ctl->NotifyParentWindowPositionChanged();
            break;  // DefWindowProc sends WM_SIZE / WM_MOVE
        case WM_SHOWWINDOW:
            if (g_ctl) g_ctl->put_IsVisible(w ? TRUE : FALSE);
            if (w) trackRoot();
            break;
        case WM_SETFOCUS:
            if (g_ctl) g_ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH b = CreateSolidBrush(backgroundColor());
            FillRect((HDC)w, &rc, b);
            DeleteObject(b);
            return 1;
        }
        case WM_NOTIFY: {
            const NMHDR* nm = (const NMHDR*)l;
            if (nm && LOWORD(nm->code) == DMN_CLOSE) {
                // Notepad++ hides the panel itself; just stop rendering
                if (g_ctl) g_ctl->put_IsVisible(FALSE);
                log(L"panel closed");
            }
            break;
        }
        case WM_DESTROY:
            if (g_rootSubclassed) RemoveWindowSubclass(g_rootSubclassed, RootSubclassProc, 1);
            g_rootSubclassed = nullptr;
            releaseWebView();
            g_host = nullptr;
            g_registered = false;
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool ensurePanel() {
    if (g_host && g_registered) return true;
    static bool classDone = false;
    if (!classDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = g_cfg.module;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWndClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            log(L"RegisterClassEx failed " + std::to_wstring(GetLastError()));
            return false;
        }
        classDone = true;
    }
    if (!g_host) {
        // Notepad++ reparents the window into its docking container and sets the styles.
        g_host = CreateWindowExW(0, kWndClass, kPanelName, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 300, 400,
                                 g_cfg.npp._nppHandle, nullptr, g_cfg.module, nullptr);
        if (!g_host) {
            log(L"CreateWindowEx failed " + std::to_wstring(GetLastError()));
            return false;
        }
    }
    if (!g_moduleName[0]) {
        wchar_t self[MAX_PATH * 2] = {0};
        GetModuleFileNameW(g_cfg.module, self, (DWORD)(sizeof self / sizeof self[0]));
        std::wstring name = fs::path(self).filename().wstring();
        if (name.empty()) name = L"YangTreeView.dll";
        wcsncpy(g_moduleName, name.c_str(), MAX_PATH - 1);
    }
    g_tb = tTbData{};
    g_tb.hClient = g_host;
    g_tb.pszName = kPanelName;
    g_tb.dlgID = g_cfg.dlgId;
    g_tb.uMask = DWS_DF_CONT_RIGHT | DWS_ADDINFO;
    g_tb.pszAddInfo = g_caption;
    g_tb.pszModuleName = g_moduleName;
    SendMessageW(g_cfg.npp._nppHandle, NPPM_DMMREGASDCKDLG, 0, (LPARAM)&g_tb);
    g_registered = true;
    log(L"panel registered");
    return true;
}

void configure(const Config& cfg) { g_cfg = cfg; }

bool show(const fs::path& page, const std::wstring& caption) {
    if (g_shuttingDown) return false;
    bool captionChanged = caption.compare(g_caption) != 0;
    wcsncpy(g_caption, caption.c_str(), sizeof g_caption / sizeof g_caption[0] - 1);
    bool wasRegistered = g_registered;
    if (!ensurePanel()) return false;
    if (captionChanged && wasRegistered) SendMessageW(g_cfg.npp._nppHandle, NPPM_DMMUPDATEDISPINFO, 0, (LPARAM)g_host);
    SendMessageW(g_cfg.npp._nppHandle, NPPM_DMMSHOW, 0, (LPARAM)g_host);
    if (!g_cfg.createWebView) {
        log(L"shown (WebView2 skipped): " + page.filename().wstring());
        return true;
    }
    navigate(pageUrl(page));
    createWebView();
    return true;
}

void defer(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!g_host || !PostMessageW(g_host, WM_YTV_DEFER, 0, (LPARAM)p)) {
        (*p)();  // no panel window (yet): run now
        delete p;
    }
}

void themeChanged() {
    bool dark = darkMode();
    log(std::wstring(L"theme ") + (dark ? L"dark" : L"light"));
    if (g_ctl) {
        if (auto c2 = g_ctl.query<ICoreWebView2Controller2>(IID_ICoreWebView2Controller2)) {
            COLORREF c = backgroundColor();
            COREWEBVIEW2_COLOR bg = {255, GetRValue(c), GetGValue(c), GetBValue(c)};
            c2->put_DefaultBackgroundColor(bg);
        }
    }
    if (g_wv) g_wv->PostWebMessageAsJson(dark ? L"{\"type\":\"theme\",\"dark\":true}" : L"{\"type\":\"theme\",\"dark\":false}");
    if (g_host) InvalidateRect(g_host, nullptr, TRUE);
}

void postStatus(const std::wstring& text) {
    log(L"status: " + text);
    if (!g_wv) return;
    std::wstring j = L"{\"type\":\"status\",\"text\":\"";
    for (wchar_t c : text) {
        if (c == L'"' || c == L'\\') { j += L'\\'; j += c; }
        else if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", (unsigned)c); j += b; }
        else j += c;
    }
    j += L"\"}";
    g_wv->PostWebMessageAsJson(j.c_str());
}

void hide() {
    if (g_host && g_registered) SendMessageW(g_cfg.npp._nppHandle, NPPM_DMMHIDE, 0, (LPARAM)g_host);
}

bool isVisible() { return g_host && IsWindowVisible(g_host); }

void shutdown() {
    g_shuttingDown = true;
    if (g_rootSubclassed) RemoveWindowSubclass(g_rootSubclassed, RootSubclassProc, 1);
    g_rootSubclassed = nullptr;
    releaseWebView();
}

}  // namespace dock
