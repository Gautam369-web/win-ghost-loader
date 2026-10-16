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
        wchar_t systemMessage[700] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, systemMessage, 700, nullptr);
        swprintf(details, 1200, L"%ls\n\nWindows error %lu: %ls", message, error, systemMessage);
    } else {
        swprintf(details, 1200, L"%ls", message);
    }
    MessageBoxW(nullptr, details, L"Veda could not start", MB_OK | MB_ICONERROR);
    return 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE running = CreateMutexW(nullptr, FALSE, L"Local\\VedaClientLauncher");
    if (!running) return Fail(L"Could not create the launcher lock.", GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Veda is already running. Press F7 to show its overlay.\n"
            L"To restart, press Alt+Q and wait for Veda to exit first.", L"Veda", MB_OK | MB_ICONINFORMATION);
        CloseHandle(running);
        return 0;
    }
    wchar_t dllPath[32768] = {};
    DWORD length = GetModuleFileNameW(nullptr, dllPath, 32768);
    if (!length || length >= 32768) {
        CloseHandle(running);
        return Fail(L"Could not find the Veda installation folder.");
    }
    wchar_t* fileName = wcsrchr(dllPath, L'\\');
    if (!fileName || static_cast<size_t>(fileName - dllPath) + 19 >= 32768) {
        CloseHandle(running);
        return Fail(L"The Veda installation path is invalid or too long.");
    }
    wcscpy(fileName + 1, L"ScreenCapture.dll");
    // Resolve private runtime dependencies beside the DLL, independent of the working directory.
    HMODULE library = LoadLibraryExW(dllPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) {
        DWORD error = GetLastError();
        CloseHandle(running);
        return Fail(L"Keep Veda.exe, ScreenCapture.dll and libwinpthread-1.dll together.\n"
            L"Extract the complete ZIP into a writable folder before starting.", error);
    }
    auto start = reinterpret_cast<StartHookFn>(GetProcAddress(library, "StartHook"));
    if (!start) {
        DWORD error = GetLastError();
        FreeLibrary(library);
        CloseHandle(running);
        return Fail(L"ScreenCapture.dll is incompatible. Extract the matching client package.", error);
    }
    // StartHook owns its message loop and drains workers before returning on Alt+Q.
    char arguments[] = "";
    start(nullptr, instance, arguments, SW_SHOWNORMAL);
    FreeLibrary(library);
    CloseHandle(running);
    return 0;
}
