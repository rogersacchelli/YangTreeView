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

// Only the header is read (SCNotification starts with an NMHDR).
struct SCNotification {
    NMHDR nmhdr;
};

// Docking (Docking.h / dockingResource.h)
struct tTbData {
    HWND hClient;                // client window (our panel)
    const wchar_t* pszName;      // tab / caption name
    int dlgID;                   // index of the menu item that shows the panel
    UINT uMask;                  // DWS_* flags
    HICON hIconTab;              // with DWS_ICONTAB
    const wchar_t* pszAddInfo;   // with DWS_ADDINFO: shown after the name in the caption
    RECT rcFloat;                // filled in by Notepad++
    int iPrevCont;               // filled in by Notepad++
    const wchar_t* pszModuleName;  // plugin DLL file name, used to save the panel state
};
#define CONT_LEFT 0
#define CONT_RIGHT 1
#define CONT_TOP 2
#define CONT_BOTTOM 3
#define DWS_ICONTAB 0x00000001
#define DWS_ICONBAR 0x00000002
#define DWS_ADDINFO 0x00000004
#define DWS_DF_CONT_LEFT (CONT_LEFT << 28)
#define DWS_DF_CONT_RIGHT (CONT_RIGHT << 28)
#define DWS_DF_CONT_TOP (CONT_TOP << 28)
#define DWS_DF_CONT_BOTTOM (CONT_BOTTOM << 28)
#define DMN_FIRST 1050
#define DMN_CLOSE (DMN_FIRST + 1)  // WM_NOTIFY to hClient; compare LOWORD(code)

// Notepad++ messages
#define NPPMSG (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_DMMSHOW (NPPMSG + 30)
#define NPPM_DMMHIDE (NPPMSG + 31)
#define NPPM_DMMUPDATEDISPINFO (NPPMSG + 32)
#define NPPM_DMMREGASDCKDLG (NPPMSG + 33)
#define NPPM_SETMENUITEMCHECK (NPPMSG + 40)
#define NPPM_GETFULLPATHFROMBUFFERID (NPPMSG + 58)
#define NPPM_GETPLUGINSCONFIGDIR (NPPMSG + 46)
#define NPPM_GETSHORTCUTBYCMDID (NPPMSG + 76)
#define NPPM_DOOPEN (NPPMSG + 77)
#define NPPM_ISDARKMODEENABLED (NPPMSG + 107)      // Npp 8.4.1+
#define NPPM_GETNPPSETTINGSDIRPATH (NPPMSG + 119)  // Npp 8.6.8+

// Notepad++ notifications (beNotified)
#define NPPN_FIRST 1000
#define NPPN_READY (NPPN_FIRST + 1)
#define NPPN_FILESAVED (NPPN_FIRST + 8)
#define NPPN_SHUTDOWN (NPPN_FIRST + 9)
#define NPPN_BUFFERACTIVATED (NPPN_FIRST + 10)
#define NPPN_BEFORESHUTDOWN (NPPN_FIRST + 19)
#define NPPN_CANCELSHUTDOWN (NPPN_FIRST + 20)
#define NPPN_DARKMODECHANGED (NPPN_FIRST + 27)  // Npp 8.4.1+

#define RUNCOMMAND_USER (WM_USER + 3000)
#define NPPM_GETFULLCURRENTPATH (RUNCOMMAND_USER + 1)

// Scintilla messages
#define SCI_GETLENGTH 2006
#define SCI_GETCODEPAGE 2137
#define SCI_GETTEXT 2182
#define SCI_GOTOLINE 2024
#define SCI_VISIBLEFROMDOCLINE 2220
#define SCI_ENSUREVISIBLEENFORCEPOLICY 2234
#define SCI_LINESONSCREEN 2370
#define SCI_SETFIRSTVISIBLELINE 2613
#define SC_CP_UTF8 65001
