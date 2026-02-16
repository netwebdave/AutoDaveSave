/*
Copyright 2026 - Dave Stewart

Licensed under the Apache License, Version 2.0 (the "License");
Licensed use requires compliance with the License.
A copy of the License is available at LICENSE or at:
http://www.apache.org/licenses/LICENSE-2.0
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <sstream>
#include <cstdint>

// ================================
// Notepad++ Messages and Events
// ================================
#define NPPMSG                  (WM_USER + 1000)
#define NPPM_MENUCOMMAND        (NPPMSG + 48)
#define NPPM_SETMENUITEMCHECK   (NPPMSG + 40)

#define NPPN_FIRST              1000
#define NPPN_READY              (NPPN_FIRST + 1)

// Notepad++ command: File -> Save All (typical builds)
#define IDM_FILE                41000
#define CMD_SAVEALL             (IDM_FILE + 7)   // 41007 Save All

// ================================
// Notepad++ Plugin API Types
// ================================
struct ShortcutKey
{
    bool  isCtrl = false;
    bool  isAlt = false;
    bool  isShift = false;
    UCHAR key = 0;
};

struct FuncItem
{
    wchar_t _itemName[64];
    void (*_pFunc)();
    int _cmdID;              // Assigned by Notepad++
    bool _init2Check;        // Initial check state
    ShortcutKey* _pShKey;    // Optional shortcut key
};

struct NppData
{
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};

struct SCNotification
{
    NMHDR nmhdr;
};

// ================================
// Globals
// ================================
static HINSTANCE g_hInst = nullptr;
static HWND g_hNppWnd = nullptr;

// Autosave timer uses TIMERPROC
static UINT_PTR g_autosaveTimerId = 0;

// Debug window uses WM_TIMER refresh
static HWND g_hDbgWnd = nullptr;
static HWND g_hDbgEdit = nullptr;
static UINT_PTR g_dbgTimerId = 0;

// About window
static HWND g_hAboutWnd = nullptr;
static HWND g_hAboutEdit = nullptr;
static HWND g_hAboutBtnRepo = nullptr;
static HWND g_hAboutBtnLinkedIn = nullptr;

// ---------------- HARD-CODED DEFAULTS ----------------
static int  g_minutes = 3;
static bool g_enabled = true;      // Start autosave on startup
static bool g_debug = false;       // Debug window hidden on startup

// Timer bookkeeping for debug countdown
static DWORD     g_intervalMs = 0;
static ULONGLONG g_nextTick = 0;

// Debug telemetry
static bool       g_lastSaveValid = false;
static SYSTEMTIME g_lastSaveLocal{};
static bool       g_lastErrValid = false;
static DWORD      g_lastErrCode = 0;

// Links
static constexpr const wchar_t* kRepoUrl = L"https://github.com/netwebdave/AutoDaveSave";
static constexpr const wchar_t* kLinkedInUrl = L"https://www.linkedin.com/in/dsii/";

// UI constants
static constexpr int kDbgTimerRefreshMs = 1000;
static constexpr UINT_PTR kDbgTimerId = 9001;

// Menu indices
enum : int
{
    FUNC_TOGGLE = 0,
    FUNC_1MIN,
    FUNC_3MIN,
    FUNC_10MIN,
    FUNC_DEBUG,
    FUNC_ABOUT,
    FUNC_COUNT
};

static FuncItem g_items[FUNC_COUNT];

// ================================
// Helpers
// ================================
static DWORD ComputeIntervalMs(const int mins)
{
    const int safeMins = (mins <= 0) ? 1 : mins;
    return static_cast<DWORD>(safeMins) * 60u * 1000u;
}

static std::wstring FormatMMSS(const DWORD totalSeconds)
{
    const DWORD m = totalSeconds / 60u;
    const DWORD s = totalSeconds % 60u;

    std::wstringstream ss;
    ss << m << L"m " << s << L"s";
    return ss.str();
}

static std::wstring FormatHHMMSS(const SYSTEMTIME& st)
{
    std::wstringstream ss;
    ss << (st.wHour < 10 ? L"0" : L"") << st.wHour << L":"
        << (st.wMinute < 10 ? L"0" : L"") << st.wMinute << L":"
        << (st.wSecond < 10 ? L"0" : L"") << st.wSecond;
    return ss.str();
}

// Initial checkmarks used while Notepad++ builds the Plugins menu
static void UpdateInitChecks()
{
    g_items[FUNC_TOGGLE]._init2Check = g_enabled;
    g_items[FUNC_1MIN]._init2Check = (g_minutes == 1);
    g_items[FUNC_3MIN]._init2Check = (g_minutes == 3);
    g_items[FUNC_10MIN]._init2Check = (g_minutes == 10);
    g_items[FUNC_DEBUG]._init2Check = g_debug;
    g_items[FUNC_ABOUT]._init2Check = false;
}

// Runtime checkmarks after Notepad++ assigns _cmdID
static void UpdateRuntimeChecks()
{
    if (!g_hNppWnd) return;

    SendMessageW(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_TOGGLE]._cmdID, (LPARAM)(g_enabled ? TRUE : FALSE));
    SendMessageW(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_1MIN]._cmdID, (LPARAM)(g_minutes == 1 ? TRUE : FALSE));
    SendMessageW(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_3MIN]._cmdID, (LPARAM)(g_minutes == 3 ? TRUE : FALSE));
    SendMessageW(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_10MIN]._cmdID, (LPARAM)(g_minutes == 10 ? TRUE : FALSE));
    SendMessageW(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_DEBUG]._cmdID, (LPARAM)(g_debug ? TRUE : FALSE));
}

static void ApplyChecks()
{
    UpdateInitChecks();
    UpdateRuntimeChecks();
}

// ================================
// Autosave Timer
// ================================
static void StopAutosaveTimer()
{
    if (!g_autosaveTimerId) return;
    KillTimer(nullptr, g_autosaveTimerId);
    g_autosaveTimerId = 0;
}

void CALLBACK AutosaveTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_enabled) return;
    if (!g_hNppWnd) return;

    g_lastErrValid = false;
    g_lastErrCode = 0;

    if (!PostMessageW(g_hNppWnd, NPPM_MENUCOMMAND, 0, CMD_SAVEALL))
    {
        g_lastErrValid = true;
        g_lastErrCode = GetLastError();
    }
    else
    {
        g_lastSaveValid = true;
        GetLocalTime(&g_lastSaveLocal);
    }

    const ULONGLONG now = GetTickCount64();
    g_nextTick = now + g_intervalMs;

    if (g_debug && g_hDbgWnd)
        PostMessageW(g_hDbgWnd, WM_TIMER, (WPARAM)g_dbgTimerId, 0);
}

static void StartAutosaveTimer()
{
    g_intervalMs = ComputeIntervalMs(g_minutes);

    StopAutosaveTimer();
    g_autosaveTimerId = SetTimer(nullptr, 0, g_intervalMs, AutosaveTimerProc);

    const ULONGLONG now = GetTickCount64();
    g_nextTick = now + g_intervalMs;
}

// ================================
// Debug Window (Resizable, Scrollable)
// ================================
static const wchar_t* DBG_CLASS = L"AutoDaveSaveDbgWnd";

static std::wstring BuildDebugText()
{
    std::wstringstream ss;

    ss << L"Enabled: " << (g_enabled ? L"Yes" : L"No") << L"\r\n";
    ss << L"Interval: " << g_minutes << L" minute(s)\r\n";

    if (!g_enabled)
    {
        ss << L"Next autosave: n/a\r\n";
    }
    else
    {
        const ULONGLONG now = GetTickCount64();
        DWORD remainSec = 0;
        if (g_nextTick > now)
            remainSec = (DWORD)((g_nextTick - now) / 1000u);

        ss << L"Next autosave in: " << FormatMMSS(remainSec) << L"\r\n";
    }

    ss << L"Last autosave at: " << (g_lastSaveValid ? FormatHHMMSS(g_lastSaveLocal) : L"n/a") << L"\r\n";
    ss << L"Last PostMessage error: " << (g_lastErrValid ? std::to_wstring(g_lastErrCode) : L"none") << L"\r\n\r\n";

    ss << L"Notes:\r\n";
    ss << L"- Untitled tabs can trigger Save As dialogs.\r\n";
    ss << L"- Debug refresh interval: 1 second.\r\n";

    return ss.str();
}

static void SetDbgText(const std::wstring& text)
{
    if (!g_hDbgEdit) return;
    SetWindowTextW(g_hDbgEdit, text.c_str());
}

static void SizeDebugControls(HWND hwnd)
{
    if (!g_hDbgEdit) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int pad = 10;
    const int w = (rc.right - rc.left) - (pad * 2);
    const int h = (rc.bottom - rc.top) - (pad * 2);

    MoveWindow(g_hDbgEdit, pad, pad, (w > 10 ? w : 10), (h > 10 ? h : 10), TRUE);
}

static LRESULT CALLBACK DbgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;

    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hDbgEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL |
            ES_AUTOHSCROLL,
            0, 0, 0, 0,
            hwnd,
            (HMENU)2001,
            g_hInst,
            nullptr
        );

        if (g_hDbgEdit && hFont)
            SendMessageW(g_hDbgEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        SizeDebugControls(hwnd);
        SetDbgText(BuildDebugText());
        return 0;
    }

    case WM_SIZE:
    {
        SizeDebugControls(hwnd);
        return 0;
    }

    case WM_TIMER:
    {
        if ((UINT_PTR)wParam != g_dbgTimerId) return 0;
        if (!g_debug) return 0;

        SetDbgText(BuildDebugText());
        return 0;
    }

    case WM_CLOSE:
    {
        g_debug = false;
        ApplyChecks();

        if (g_dbgTimerId)
        {
            KillTimer(hwnd, g_dbgTimerId);
            g_dbgTimerId = 0;
        }

        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
    {
        g_hDbgWnd = nullptr;
        g_hDbgEdit = nullptr;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureDbgClassRegistered()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DbgWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = DBG_CLASS;

    RegisterClassExW(&wc);
}

static void ShowDebugWindow()
{
    if (g_hDbgWnd)
    {
        ShowWindow(g_hDbgWnd, SW_SHOW);
        SetForegroundWindow(g_hDbgWnd);
        return;
    }

    EnsureDbgClassRegistered();

    int x = 220, y = 220;
    if (g_hNppWnd)
    {
        RECT r{};
        if (GetWindowRect(g_hNppWnd, &r))
        {
            x = r.left + 40;
            y = r.top + 80;
        }
    }

    g_hDbgWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        DBG_CLASS,
        L"AutoDaveSave Debug",
        WS_OVERLAPPEDWINDOW,
        x, y, 560, 320,
        g_hNppWnd,
        nullptr,
        g_hInst,
        nullptr
    );

    if (!g_hDbgWnd) return;

    ShowWindow(g_hDbgWnd, SW_SHOW);

    g_dbgTimerId = kDbgTimerId;
    SetTimer(g_hDbgWnd, g_dbgTimerId, kDbgTimerRefreshMs, nullptr);

    PostMessageW(g_hDbgWnd, WM_TIMER, (WPARAM)g_dbgTimerId, 0);
}

static void HideDebugWindow()
{
    if (!g_hDbgWnd) return;

    if (g_dbgTimerId)
    {
        KillTimer(g_hDbgWnd, g_dbgTimerId);
        g_dbgTimerId = 0;
    }

    DestroyWindow(g_hDbgWnd);
    g_hDbgWnd = nullptr;
    g_hDbgEdit = nullptr;
}

// ================================
// About Window (Fix button cut-off, add LinkedIn button)
// ================================
static const wchar_t* ABOUT_CLASS = L"AutoDaveSaveAboutWnd";

// About control IDs
static constexpr int ID_ABOUT_EDIT = 1001;
static constexpr int ID_ABOUT_REPO = 1002;
static constexpr int ID_ABOUT_LI = 1003;

static void OpenUrl(const wchar_t* url)
{
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static std::wstring AboutText()
{
    std::wstring t;
    t += L"AutoDaveSave\r\n\r\n";

    t += L"License\r\n";
    t += L"- Apache License 2.0 (see LICENSE)\r\n\r\n";

    t += L"Repository\r\n";
    t += L"- https://github.com/netwebdave/AutoDaveSave\r\n\r\n";

    t += L"How to use\r\n";
    t += L"1) Plugins > AutoDaveSave > Start or Stop Autosave\r\n";
    t += L"2) Select interval: 1, 3, or 10 minutes\r\n";
    t += L"3) Optional: Show Timer Selection (Debug)\r\n\r\n";

    t += L"Notes\r\n";
    t += L"- Untitled tabs can trigger Save As prompts when Save All runs\r\n\r\n";

    // Non-corny, short, and relevant line
    t += L"Contact\r\n";
    t += L"- LinkedIn: dsii (connect for collaboration)\r\n";

    return t;
}

static void SizeAboutControls(HWND hwnd)
{
    if (!g_hAboutEdit || !g_hAboutBtnRepo || !g_hAboutBtnLinkedIn) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int pad = 12;
    const int btnH = 28;
    const int btnGap = 10;

    const int w = (rc.right - rc.left) - (pad * 2);
    const int h = (rc.bottom - rc.top) - (pad * 3) - btnH;

    // Edit takes top, buttons at bottom
    MoveWindow(g_hAboutEdit, pad, pad, (w > 10 ? w : 10), (h > 10 ? h : 10), TRUE);

    const int btnW1 = 220;
    const int btnW2 = 180;
    const int y = pad + h + pad;

    MoveWindow(g_hAboutBtnRepo, pad, y, btnW1, btnH, TRUE);
    MoveWindow(g_hAboutBtnLinkedIn, pad + btnW1 + btnGap, y, btnW2, btnH, TRUE);
}

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;

    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hAboutEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            AboutText().c_str(),
            WS_CHILD | WS_VISIBLE |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0,
            hwnd,
            (HMENU)ID_ABOUT_EDIT,
            g_hInst,
            nullptr
        );

        g_hAboutBtnRepo = CreateWindowExW(
            0,
            L"BUTTON",
            L"Open GitHub Repository",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0,
            hwnd,
            (HMENU)ID_ABOUT_REPO,
            g_hInst,
            nullptr
        );

        g_hAboutBtnLinkedIn = CreateWindowExW(
            0,
            L"BUTTON",
            L"Open LinkedIn",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0,
            hwnd,
            (HMENU)ID_ABOUT_LI,
            g_hInst,
            nullptr
        );

        if (g_hAboutEdit && hFont) SendMessageW(g_hAboutEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (g_hAboutBtnRepo && hFont) SendMessageW(g_hAboutBtnRepo, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (g_hAboutBtnLinkedIn && hFont) SendMessageW(g_hAboutBtnLinkedIn, WM_SETFONT, (WPARAM)hFont, TRUE);

        SizeAboutControls(hwnd);
        return 0;
    }

    case WM_SIZE:
    {
        SizeAboutControls(hwnd);
        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);

        if (id == ID_ABOUT_REPO)
        {
            OpenUrl(kRepoUrl);
            return 0;
        }
        if (id == ID_ABOUT_LI)
        {
            OpenUrl(kLinkedInUrl);
            return 0;
        }

        return 0;
    }

    case WM_CLOSE:
    {
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
    {
        g_hAboutWnd = nullptr;
        g_hAboutEdit = nullptr;
        g_hAboutBtnRepo = nullptr;
        g_hAboutBtnLinkedIn = nullptr;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureAboutClassRegistered()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = ABOUT_CLASS;

    RegisterClassExW(&wc);
}

static void ShowAboutWindow()
{
    if (g_hAboutWnd)
    {
        ShowWindow(g_hAboutWnd, SW_SHOW);
        SetForegroundWindow(g_hAboutWnd);
        return;
    }

    EnsureAboutClassRegistered();

    // Increased height to ensure bottom buttons are fully visible.
    // Also uses WS_OVERLAPPEDWINDOW for manual resize.
    g_hAboutWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        ABOUT_CLASS,
        L"About AutoDaveSave",
        WS_OVERLAPPEDWINDOW,
        240, 240, 640, 460,
        g_hNppWnd,
        nullptr,
        g_hInst,
        nullptr
    );

    if (!g_hAboutWnd) return;
    ShowWindow(g_hAboutWnd, SW_SHOW);
}

// ================================
// Menu Actions
// ================================
static void ToggleAutosave()
{
    g_enabled = !g_enabled;

    if (g_enabled) StartAutosaveTimer();
    else StopAutosaveTimer();

    ApplyChecks();

    if (g_debug)
        ShowDebugWindow();

    if (g_debug && g_hDbgWnd)
        PostMessageW(g_hDbgWnd, WM_TIMER, (WPARAM)g_dbgTimerId, 0);
}

static void SetMinutes(const int m)
{
    g_minutes = (m <= 0) ? 1 : m;

    if (g_enabled)
        StartAutosaveTimer();

    ApplyChecks();

    if (g_debug)
        ShowDebugWindow();
}

static void Set1() { SetMinutes(1); }
static void Set3() { SetMinutes(3); }
static void Set10() { SetMinutes(10); }

static void ToggleDebug()
{
    g_debug = !g_debug;

    if (g_debug) ShowDebugWindow();
    else HideDebugWindow();

    ApplyChecks();
}

static void ShowAbout()
{
    ShowAboutWindow();
}

// ================================
// Cleanup
// ================================
static void Cleanup()
{
    StopAutosaveTimer();

    if (g_hDbgWnd)
        HideDebugWindow();

    if (g_hAboutWnd)
        DestroyWindow(g_hAboutWnd);

    g_hAboutWnd = nullptr;
    g_hAboutEdit = nullptr;
    g_hAboutBtnRepo = nullptr;
    g_hAboutBtnLinkedIn = nullptr;

    g_hNppWnd = nullptr;
}

// ================================
// Notepad++ Required Exports
// ================================
extern "C" __declspec(dllexport) void setInfo(void* data)
{
    const NppData* pData = static_cast<const NppData*>(data);
    g_hNppWnd = pData ? pData->_nppHandle : nullptr;

    // Hard-coded defaults applied on every startup
    g_minutes = 3;
    g_enabled = true;
    g_debug = false;

    ZeroMemory(g_items, sizeof(g_items));

    wcscpy_s(g_items[FUNC_TOGGLE]._itemName, L"Start or Stop Autosave");
    g_items[FUNC_TOGGLE]._pFunc = ToggleAutosave;

    wcscpy_s(g_items[FUNC_1MIN]._itemName, L"Set Autosave to 1 Minute");
    g_items[FUNC_1MIN]._pFunc = Set1;

    wcscpy_s(g_items[FUNC_3MIN]._itemName, L"Set Autosave to 3 Minutes");
    g_items[FUNC_3MIN]._pFunc = Set3;

    wcscpy_s(g_items[FUNC_10MIN]._itemName, L"Set Autosave to 10 Minutes");
    g_items[FUNC_10MIN]._pFunc = Set10;

    wcscpy_s(g_items[FUNC_DEBUG]._itemName, L"Show Timer Selection (Debug)");
    g_items[FUNC_DEBUG]._pFunc = ToggleDebug;

    wcscpy_s(g_items[FUNC_ABOUT]._itemName, L"About AutoDaveSave");
    g_items[FUNC_ABOUT]._pFunc = ShowAbout;

    UpdateInitChecks();

    // Hard-coded startup behavior: start autosave immediately
    StartAutosaveTimer();
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"AutoDaveSave";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count)
{
    if (count) *count = FUNC_COUNT;
    UpdateInitChecks();
    return g_items;
}

extern "C" __declspec(dllexport) void beNotified(void* notifyCode)
{
    const SCNotification* scn = static_cast<const SCNotification*>(notifyCode);

    if (scn && scn->nmhdr.code == NPPN_READY)
        UpdateRuntimeChecks();
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM)
{
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        g_hInst = (HINSTANCE)hModule;
    else if (reason == DLL_PROCESS_DETACH)
        Cleanup();

    return TRUE;
}
