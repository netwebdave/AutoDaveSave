/*
Copyright 2026 - Dave Stewart

Licensed under the Apache License, Version 2.0 (the "License");
Licensed use requires compliance with the License.
A copy of the License is available at LICENSE or at:
http://www.apache.org/licenses/LICENSE-2.0
*/

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>   // ShellExecuteW
#include <string>
#include <sstream>

// ---------------- Notepad++ Messages ----------------
#define NPPMSG                  (WM_USER + 1000)
#define NPPM_MENUCOMMAND        (NPPMSG + 48)
#define NPPM_SETMENUITEMCHECK   (NPPMSG + 40)

#define NPPN_FIRST              1000
#define NPPN_READY              (NPPN_FIRST + 1)

// ---------------- Notepad++ Command IDs ----------------
#define IDM_FILE                41000
#define CMD_SAVEALL             (IDM_FILE + 7)   // 41007 Save All

// ---------------- Globals ----------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hNppWnd = nullptr;

// Autosave timer uses TIMERPROC
static UINT_PTR g_autosaveTimerId = 0;

// Debug UI window owns countdown timer (WM_TIMER in its WndProc)
static HWND g_hDbgWnd = nullptr;
static HWND g_hDbgLabel = nullptr;
static UINT_PTR g_dbgTimerId = 0;

// About UI
static HWND g_hAboutWnd = nullptr;

static int  g_minutes = 3;
static bool g_enabled = true;
static bool g_debug = false;

static DWORD     g_intervalMs = 0;
static ULONGLONG g_nextTick = 0;

// Repository link
static const wchar_t* kRepoUrl = L"https://github.com/USERNAME/AutoDaveSave";

// ---------------- Menu indices ----------------
enum {
    FUNC_TOGGLE = 0,
    FUNC_1MIN,
    FUNC_3MIN,
    FUNC_10MIN,
    FUNC_DEBUG,
    FUNC_ABOUT,
    FUNC_COUNT
};

// ---------------- Notepad++ plugin structs ----------------
struct FuncItem {
    wchar_t _itemName[64];
    void (*_pFunc)();
    int _cmdID;
    bool _init2Check;
    void* _pShKey;
};

struct NppData {
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};

struct SCNotification {
    NMHDR nmhdr;
};

static FuncItem g_items[FUNC_COUNT];

// ---------------- Helpers ----------------
static DWORD ComputeIntervalMs(int mins)
{
    if (mins <= 0) mins = 1;
    return (DWORD)(mins * 60 * 1000);
}

static std::wstring FormatMMSS(DWORD totalSeconds)
{
    DWORD m = totalSeconds / 60;
    DWORD s = totalSeconds % 60;

    std::wstringstream ss;
    ss << m << L"m " << s << L"s";
    return ss.str();
}

static void UpdateInitChecks()
{
    g_items[FUNC_TOGGLE]._init2Check = g_enabled;
    g_items[FUNC_1MIN]._init2Check = (g_minutes == 1);
    g_items[FUNC_3MIN]._init2Check = (g_minutes == 3);
    g_items[FUNC_10MIN]._init2Check = (g_minutes == 10);
    g_items[FUNC_DEBUG]._init2Check = g_debug;
    g_items[FUNC_ABOUT]._init2Check = false;
}

static void UpdateRuntimeChecks()
{
    if (!g_hNppWnd) return;

    SendMessage(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_TOGGLE]._cmdID, (LPARAM)(g_enabled ? TRUE : FALSE));
    SendMessage(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_1MIN]._cmdID, (LPARAM)(g_minutes == 1 ? TRUE : FALSE));
    SendMessage(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_3MIN]._cmdID, (LPARAM)(g_minutes == 3 ? TRUE : FALSE));
    SendMessage(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_10MIN]._cmdID, (LPARAM)(g_minutes == 10 ? TRUE : FALSE));
    SendMessage(g_hNppWnd, NPPM_SETMENUITEMCHECK, (WPARAM)g_items[FUNC_DEBUG]._cmdID, (LPARAM)(g_debug ? TRUE : FALSE));
}

static void ApplyChecks()
{
    UpdateInitChecks();
    UpdateRuntimeChecks();
}

// ---------------- Debug window ----------------
static const wchar_t* DBG_CLASS = L"AutoDaveSaveDbgWnd";

static void SetDbgText(const std::wstring& text)
{
    if (g_hDbgLabel)
        SetWindowTextW(g_hDbgLabel, text.c_str());
}

static LRESULT CALLBACK DbgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hDbgLabel = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 10, 420, 70,
            hwnd,
            nullptr,
            g_hInst,
            nullptr
        );

        if (g_hDbgLabel && hFont)
            SendMessage(g_hDbgLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        return 0;
    }

    case WM_TIMER:
    {
        if ((UINT_PTR)wParam == g_dbgTimerId)
        {
            if (!g_debug)
                return 0;

            if (!g_enabled)
            {
                SetDbgText(L"AutoDaveSave paused.");
                return 0;
            }

            ULONGLONG now = GetTickCount64();
            DWORD remainSec = 0;
            if (g_nextTick > now)
                remainSec = (DWORD)((g_nextTick - now) / 1000);

            std::wstring line1 = L"Interval: " + std::to_wstring(g_minutes) + L" minute(s)";
            std::wstring line2 = L"Next autosave in: " + FormatMMSS(remainSec);
            std::wstring out = line1 + L"\r\n" + line2;

            SetDbgText(out);
        }
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
        g_hDbgLabel = nullptr;
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
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
    if (g_hDbgWnd) {
        ShowWindow(g_hDbgWnd, SW_SHOW);
        SetForegroundWindow(g_hDbgWnd);
        return;
    }

    EnsureDbgClassRegistered();

    g_hDbgWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        DBG_CLASS,
        L"AutoDaveSave Debug",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        220, 220, 460, 140,
        g_hNppWnd,
        nullptr,
        g_hInst,
        nullptr
    );

    if (!g_hDbgWnd)
        return;

    ShowWindow(g_hDbgWnd, SW_SHOW);

    if (g_dbgTimerId)
        KillTimer(g_hDbgWnd, g_dbgTimerId);

    g_dbgTimerId = 9001;
    SetTimer(g_hDbgWnd, g_dbgTimerId, 1000, nullptr);

    PostMessage(g_hDbgWnd, WM_TIMER, (WPARAM)g_dbgTimerId, 0);
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
    g_hDbgLabel = nullptr;
}

// ---------------- About window ----------------
static const wchar_t* ABOUT_CLASS = L"AutoDaveSaveAboutWnd";
static HWND g_hAboutText = nullptr;
static HWND g_hAboutBtn = nullptr;

static void OpenRepoUrl()
{
    ShellExecuteW(nullptr, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

static std::wstring AboutText()
{
    std::wstring t;
    t += L"AutoDaveSave\n\n";
    t += L"Features\n";
    t += L"- Silent Save All at selected interval\n";
    t += L"- Menu checkmarks show active interval and debug state\n";
    t += L"- Debug window shows countdown to next autosave\n\n";
    t += L"How to use\n";
    t += L"1) Plugins > AutoDaveSave > Start or Stop Autosave\n";
    t += L"2) Select interval: 1, 3, or 10 minutes\n";
    t += L"3) Optional: enable Show Timer Selection (Debug)\n\n";
    t += L"Notes\n";
    t += L"- Untitled tabs can trigger Save As prompts when Save All runs\n";
    t += L"- Save files at least once to ensure silent autosave\n";
    return t;
}

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hAboutText = CreateWindowExW(
            0,
            L"STATIC",
            AboutText().c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 12, 520, 200,
            hwnd,
            (HMENU)1001,
            g_hInst,
            nullptr
        );

        g_hAboutBtn = CreateWindowExW(
            0,
            L"BUTTON",
            L"Open GitHub Repository",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 220, 220, 28,
            hwnd,
            (HMENU)1002,
            g_hInst,
            nullptr
        );

        if (g_hAboutText && hFont) SendMessage(g_hAboutText, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (g_hAboutBtn && hFont)  SendMessage(g_hAboutBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        if (id == 1002)
        {
            OpenRepoUrl();
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
        g_hAboutText = nullptr;
        g_hAboutBtn = nullptr;
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
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
    if (g_hAboutWnd) {
        ShowWindow(g_hAboutWnd, SW_SHOW);
        SetForegroundWindow(g_hAboutWnd);
        return;
    }

    EnsureAboutClassRegistered();

    g_hAboutWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        ABOUT_CLASS,
        L"About AutoDaveSave",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        240, 240, 560, 310,
        g_hNppWnd,
        nullptr,
        g_hInst,
        nullptr
    );

    if (!g_hAboutWnd) return;

    ShowWindow(g_hAboutWnd, SW_SHOW);
}

// ---------------- Autosave timer ----------------
void CALLBACK AutosaveTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_enabled) return;
    if (!g_hNppWnd) return;

    PostMessage(g_hNppWnd, NPPM_MENUCOMMAND, 0, CMD_SAVEALL);

    ULONGLONG now = GetTickCount64();
    g_nextTick = now + g_intervalMs;

    if (g_debug && g_hDbgWnd)
        PostMessage(g_hDbgWnd, WM_TIMER, (WPARAM)g_dbgTimerId, 0);
}

static void StartAutosaveTimer()
{
    g_intervalMs = ComputeIntervalMs(g_minutes);

    if (g_autosaveTimerId)
        KillTimer(nullptr, g_autosaveTimerId);

    g_autosaveTimerId = SetTimer(nullptr, 0, g_intervalMs, AutosaveTimerProc);

    ULONGLONG now = GetTickCount64();
    g_nextTick = now + g_intervalMs;
}

static void StopAutosaveTimer()
{
    if (!g_autosaveTimerId) return;

    KillTimer(nullptr, g_autosaveTimerId);
    g_autosaveTimerId = 0;
}

// ---------------- Menu actions ----------------
static void ToggleAutosave()
{
    g_enabled = !g_enabled;

    if (g_enabled) StartAutosaveTimer();
    else StopAutosaveTimer();

    ApplyChecks();

    if (g_debug && g_hDbgWnd)
        PostMessage(g_hDbgWnd, WM_TIMER, (WPARAM)g_dbgTimerId, 0);
}

static void SetMinutes(int m)
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

// ---------------- Notepad++ exports ----------------
extern "C" __declspec(dllexport)
void setInfo(void* data)
{
    NppData* pData = (NppData*)data;
    g_hNppWnd = pData ? pData->_nppHandle : nullptr;

    // Defaults
    g_minutes = 3;
    g_enabled = true;
    g_debug = false;

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
    StartAutosaveTimer();
}

extern "C" __declspec(dllexport)
const TCHAR* getName()
{
    return L"AutoDaveSave";
}

extern "C" __declspec(dllexport)
FuncItem* getFuncsArray(int* count)
{
    if (count) *count = FUNC_COUNT;
    UpdateInitChecks();
    return g_items;
}

extern "C" __declspec(dllexport)
void beNotified(void* notify)
{
    SCNotification* scn = (SCNotification*)notify;

    if (scn && scn->nmhdr.code == NPPN_READY)
        UpdateRuntimeChecks();
}

extern "C" __declspec(dllexport)
LRESULT messageProc(UINT, WPARAM, LPARAM)
{
    return TRUE;
}

extern "C" __declspec(dllexport)
BOOL isUnicode()
{
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD, LPVOID)
{
    g_hInst = (HINSTANCE)hModule;
    return TRUE;
}