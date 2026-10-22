#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>

using StartHookFn = void (CALLBACK *)(HWND, HINSTANCE, LPSTR, int);

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

// ── HOST MODE: load DLL and run hook (called from the detached child) ─────────
static int RunHost(HINSTANCE instance) {
    // Single-instance guard owned by the host process
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\VedaClientLauncher");
    if (!mutex) return Fail(L"Could not create the launcher lock.", GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0; // Another host is already running — silently exit
    }

    // Locate ScreenCapture.dll next to Veda.exe
    wchar_t dllPath[32768] = {};
    DWORD length = GetModuleFileNameW(nullptr, dllPath, 32768);
    if (!length || length >= 32768) {
        CloseHandle(mutex);
        return Fail(L"Could not find the Veda installation folder.");
    }
    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (!slash || static_cast<size_t>(slash - dllPath) + 19 >= 32768) {
        CloseHandle(mutex);
        return Fail(L"The Veda installation path is invalid or too long.");
    }
    wcscpy(slash + 1, L"ScreenCapture.dll");

    HMODULE lib = LoadLibraryExW(dllPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) {
        DWORD err = GetLastError(); CloseHandle(mutex);
        return Fail(L"Keep Veda.exe, ScreenCapture.dll and libwinpthread-1.dll together.\n"
                    L"Extract the complete ZIP into a writable folder before starting.", err);
    }

    void* raw = reinterpret_cast<void*>(GetProcAddress(lib, "StartHook"));
    auto start = reinterpret_cast<StartHookFn>(raw);
    if (!start) {
        DWORD err = GetLastError(); FreeLibrary(lib); CloseHandle(mutex);
        return Fail(L"ScreenCapture.dll is incompatible. Extract the matching client package.", err);
    }

    // Blocks here until Alt+Q is pressed inside the DLL
    char arguments[] = "";
    start(nullptr, instance, arguments, SW_SHOWNORMAL);

    FreeLibrary(lib);
    CloseHandle(mutex);
    return 0;
}

// ── LAUNCHER MODE: check single-instance, spawn host, exit immediately ────────
static int RunLauncher() {
    // Check if a host is already running
    HANDLE probe = CreateMutexW(nullptr, FALSE, L"Local\\VedaClientLauncher");
    if (!probe) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(probe);
        MessageBoxW(nullptr,
            L"Veda is already running.\n"
            L"Press F7 to show its overlay.\n"
            L"Press Alt+Q to quit.",
            L"Veda", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    // Release immediately — the host will own the mutex
    CloseHandle(probe);

    // Get path to this executable
    wchar_t selfPath[32768] = {};
    if (!GetModuleFileNameW(nullptr, selfPath, 32768)) return 0;

    // Build command line: "Veda.exe" --host
    wchar_t cmdLine[32800] = {};
    swprintf(cmdLine, 32800, L"\"%ls\" --host", selfPath);

    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // CREATE_NO_WINDOW + DETACHED_PROCESS → fully invisible, no console
    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, nullptr, &si, &pi)) {
        return Fail(L"Could not launch the Veda host process.", GetLastError());
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0; // Launcher exits immediately — host runs in background
}

// ── Entry point ───────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    // If spawned with --host flag → run as the persistent DLL host
    if (cmdLine && wcsstr(cmdLine, L"--host"))
        return RunHost(instance);
    // Otherwise → launcher: spawn host detached and exit right away
    return RunLauncher();
}
