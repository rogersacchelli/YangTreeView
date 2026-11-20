// Minimal subset of the Notepad++ plugin API (PluginInterface.h,
// Notepad_plus_msgs.h, Scintilla.h) needed by this plugin.
// Values match notepad-plus-plus/PowerEditor/src/MISC/PluginsManager and Scintilla.
#pragma once
#include <windows.h>

struct NppData {
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};

typedef void(__cdecl* PFUNCPLUGINCMD)();

struct ShortcutKey {
    bool _isCtrl;
    bool _isAlt;
    bool _isShift;
    UCHAR _key;
};

const int nbChar = 64;

struct FuncItem {
    wchar_t _itemName[nbChar];
    PFUNCPLUGINCMD _pFunc;
    int _cmdID;
    bool _init2Check;
    ShortcutKey* _pShKey;
};

struct SCNotification;  // opaque - not used

// Notepad++ messages
#define NPPMSG (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_GETPLUGINSCONFIGDIR (NPPMSG + 46)
#define NPPM_DOOPEN (NPPMSG + 77)
#define NPPM_ISDARKMODEENABLED (NPPMSG + 107)      // Npp 8.4.1+
#define NPPM_GETNPPSETTINGSDIRPATH (NPPMSG + 119)  // Npp 8.6.8+

#define RUNCOMMAND_USER (WM_USER + 3000)
#define NPPM_GETFULLCURRENTPATH (RUNCOMMAND_USER + 1)

// Scintilla messages
#define SCI_GETLENGTH 2006
#define SCI_GETCODEPAGE 2137
#define SCI_GETTEXT 2182
#define SCI_GOTOLINE 2024
#define SC_CP_UTF8 65001
