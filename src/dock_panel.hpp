// YangTreeView - docked viewer panel
// A Notepad++ dockable panel hosting a WebView2 (Edge/Chromium) control that shows the
// same HTML page the browser mode opens, so look and behaviour are identical.
#pragma once
#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

#include "npp_plugin_api.h"

namespace dock {

namespace fs = std::filesystem;

struct Config {
    NppData npp{};
    HINSTANCE module = nullptr;  // plugin DLL
    int dlgId = 0;               // funcItem index Notepad++ reruns to restore the panel
    fs::path pagesDir;           // folder holding the rendered pages (served to the WebView)
    fs::path userDataDir;        // WebView2 profile (cache, localStorage)
    bool createWebView = true;   // false: self-test without WebView2 (panel only)
    std::function<void(const std::wstring&)> log;              // diagnostics (may be empty)
    std::function<void(const std::wstring& why)> onFailure;    // WebView2 failed asynchronously
    // A string posted by the page (chrome.webview.postMessage), e.g. "popout".
    // Called from the message loop, not from inside the WebView2 event.
    std::function<void(const std::wstring& msg)> onMessage;
    // A key with Ctrl/Alt (or a non-character key) pressed while the WebView has focus.
    // Return true to consume it (use defer() for any real work).
    std::function<bool(UINT vk, bool ctrl, bool alt, bool shift)> onAccelerator;
};

// Call once (setInfo time is too early for the HWNDs to be used; first show is fine).
void configure(const Config& cfg);

// Version string of the installed WebView2 Runtime, or empty if none is usable.
std::wstring runtimeVersion();

// Shows the panel (registering it with Notepad++ the first time) and displays `page`,
// a file inside cfg.pagesDir. `caption` is shown after the panel name (e.g. the module).
// Returns false if the panel could not be created (caller falls back to the browser).
bool show(const fs::path& page, const std::wstring& caption);

void hide();
bool isVisible();

// Notepad++ dark mode changed: updates the panel background and tells the page.
void themeChanged();

// Banner text in the page (empty clears it), e.g. why an automatic refresh failed.
void postStatus(const std::wstring& text);

// Runs fn from the message loop (outside any WebView2 event handler).
void defer(std::function<void()> fn);

// NPPN_SHUTDOWN: closes the WebView before Notepad++ destroys its windows.
void shutdown();

}  // namespace dock
