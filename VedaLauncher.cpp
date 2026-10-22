#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

using StartHookFn = void (CALLBACK *)(HWND, HINSTANCE, LPSTR, int);

// ── Tray icon constants ───────────────────────────────────────────────────────
#define IDI_TRAY_ICON    1
#define WM_TRAY          (WM_APP + 10)   // notification from shell
#define ID_TRAY_SHOW     101             // menu: Show Overlay
#define ID_TRAY_QUIT     102             // menu: Quit Veda

static NOTIFYICONDATAW  g_nid   = {};
static HWND             g_hwnd  = nullptr;
static HMODULE          g_lib   = nullptr;
static HANDLE           g_mutex = nullptr;

// ── Error helper ─────────────────────────────────────────────────────────────
static int Fail(const wchar_t* message, DWORD error = 0) {
    wchar_t details[1200] = {};
    if (error) {
        wchar_t sys[700] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, sys, 700, nullptr);
        swprintf(details, 1200, L"%ls\n\nWindows error %lu: %ls", message, error, sys);
    } else {
        swprintf(details, 1200, L"%ls", message);
    }
    MessageBoxW(nullptr, details, L"Veda could not start", MB_OK | MB_ICONERROR);
    return 1;
}

// ── Add / remove tray icon ────────────────────────────────────────────────────
static void TrayAdd(HINSTANCE instance) {
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = IDI_TRAY_ICON;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIconW(instance, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Veda  |  F7=Overlay  Alt+Q=Quit");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayRemove() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// ── Show context menu ─────────────────────────────────────────────────────────
static void ShowTrayMenu() {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"Show Overlay (F7)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_QUIT, L"Quit Veda (Alt+Q)");
    SetMenuDefaultItem(hMenu, ID_TRAY_SHOW, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);   // required so menu dismisses on click-away
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(hMenu);
}

// ── Window procedure for the hidden message-only window ───────────────────────
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            ShowTrayMenu();
        else if (lp == WM_LBUTTONDBLCLK)
            // Double-click = Show Overlay (simulate F7)
            PostMessageW(hwnd, WM_COMMAND, ID_TRAY_SHOW, 0);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_SHOW:
            // Send F7 keypress to the active window to toggle the overlay
            {
                INPUT inp[2] = {};
                inp[0].type = INPUT_KEYBOARD; inp[0].ki.wVk = VK_F7;
                inp[1].type = INPUT_KEYBOARD; inp[1].ki.wVk = VK_F7;
                inp[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, inp, sizeof(INPUT));
            }
            return 0;
        case ID_TRAY_QUIT:
            // Send Alt+Q to the hook (which calls StopHook internally)
            {
                INPUT inp[4] = {};
                inp[0].type = INPUT_KEYBOARD; inp[0].ki.wVk = VK_MENU;   // Alt down
                inp[1].type = INPUT_KEYBOARD; inp[1].ki.wVk = 'Q';        // Q down
                inp[2].type = INPUT_KEYBOARD; inp[2].ki.wVk = 'Q';        // Q up
                inp[2].ki.dwFlags = KEYEVENTF_KEYUP;
                inp[3].type = INPUT_KEYBOARD; inp[3].ki.wVk = VK_MENU;   // Alt up
                inp[3].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(4, inp, sizeof(INPUT));
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry point ───────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Single-instance guard
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\VedaClientLauncher");
    if (!g_mutex) return Fail(L"Could not create the launcher lock.", GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
            L"Veda is already running.\n"
            L"Look for its icon in the system tray (bottom-right corner).\n"
            L"Right-click it to show the overlay or quit.",
            L"Veda", MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_mutex);
        return 0;
    }

    // Locate ScreenCapture.dll next to Veda.exe
    wchar_t dllPath[32768] = {};
    DWORD length = GetModuleFileNameW(nullptr, dllPath, 32768);
    if (!length || length >= 32768) {
        CloseHandle(g_mutex);
        return Fail(L"Could not find the Veda installation folder.");
    }
    wchar_t* fileName = wcsrchr(dllPath, L'\\');
    if (!fileName || static_cast<size_t>(fileName - dllPath) + 19 >= 32768) {
        CloseHandle(g_mutex);
        return Fail(L"The Veda installation path is invalid or too long.");
    }
    wcscpy(fileName + 1, L"ScreenCapture.dll");

    g_lib = LoadLibraryExW(dllPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_lib) {
        DWORD err = GetLastError(); CloseHandle(g_mutex);
        return Fail(L"Keep Veda.exe, ScreenCapture.dll and libwinpthread-1.dll together.\n"
                    L"Extract the complete ZIP into a writable folder before starting.", err);
    }

    void* rawStart = reinterpret_cast<void*>(GetProcAddress(g_lib, "StartHook"));
    auto start = reinterpret_cast<StartHookFn>(rawStart);
    if (!start) {
        DWORD err = GetLastError(); FreeLibrary(g_lib); CloseHandle(g_mutex);
        return Fail(L"ScreenCapture.dll is incompatible. Extract the matching client package.", err);
    }

    // ── Create hidden message-only window for tray icon ───────────────────────
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TrayWndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = L"VedaTrayHost";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, L"VedaTrayHost", L"Veda",
                             0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);

    // ── Add tray icon ─────────────────────────────────────────────────────────
    TrayAdd(instance);

    // ── Start the DLL hook (runs its own threads; blocks until Alt+Q) ─────────
    char arguments[] = "";
    start(nullptr, instance, arguments, SW_SHOWNORMAL);

    // ── DLL has stopped — drain any pending tray messages then exit ───────────
    TrayRemove();
    FreeLibrary(g_lib);
    DestroyWindow(g_hwnd);
    UnregisterClassW(L"VedaTrayHost", instance);
    CloseHandle(g_mutex);
    return 0;
}
