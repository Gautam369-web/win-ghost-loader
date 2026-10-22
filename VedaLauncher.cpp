#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

// ── helpers ──────────────────────────────────────────────────────────────────

static void Fail(const wchar_t* msg, DWORD err = 0) {
    wchar_t buf[1200] = {};
    if (err) {
        wchar_t sys[700] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, err, 0, sys, 700, nullptr);
        swprintf(buf, 1200, L"%ls\n\nError %lu: %ls", msg, err, sys);
    } else {
        swprintf(buf, 1200, L"%ls", msg);
    }
    MessageBoxW(nullptr, buf, L"Veda", MB_OK | MB_ICONERROR);
}

// ── find explorer.exe PID ────────────────────────────────────────────────────

static DWORD FindExplorerPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// ── inject DLL into target process ──────────────────────────────────────────

static bool InjectDll(DWORD pid, const wchar_t* dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        Fail(L"Cannot open explorer.exe.\nTry running Veda.exe as Administrator.", GetLastError());
        return false;
    }

    // Allocate memory in explorer for the DLL path string
    size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(hProc, nullptr, pathBytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        Fail(L"Cannot allocate memory in explorer.exe.", GetLastError());
        CloseHandle(hProc);
        return false;
    }

    // Write the DLL path into explorer's memory
    if (!WriteProcessMemory(hProc, remote, dllPath, pathBytes, nullptr)) {
        Fail(L"Cannot write DLL path into explorer.exe.", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Get address of LoadLibraryW in kernel32 (same in all processes on same OS)
    LPVOID loadLib = reinterpret_cast<LPVOID>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLib) {
        Fail(L"Cannot find LoadLibraryW.", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Create remote thread in explorer that calls LoadLibraryW(dllPath)
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
        remote, 0, nullptr);
    if (!hThread) {
        Fail(L"Cannot create remote thread in explorer.exe.\nTry running Veda.exe as Administrator.", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Wait for LoadLibraryW to complete (DLL is now loaded)
    WaitForSingleObject(hThread, 5000);

    // Check if DLL was loaded successfully
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (!exitCode) {
        Fail(L"ScreenCapture.dll failed to load into explorer.exe.\n"
             L"Make sure ScreenCapture.dll and libwinpthread-1.dll are in the same folder as Veda.exe.");
        return false;
    }

    return true;
}

// ── check if DLL already injected (mutex probe) ──────────────────────────────

static bool AlreadyRunning() {
    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\VedaClientLauncher");
    if (!m) return false;
    bool running = (GetLastError() == ERROR_ALREADY_EXISTS);
    CloseHandle(m);
    return running;
}

// ── entry point ──────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Single-instance check
    if (AlreadyRunning()) {
        MessageBoxW(nullptr,
            L"Veda is already running.\n"
            L"Press F7 to show its overlay.\n"
            L"Press Alt+Q to quit.",
            L"Veda", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Find explorer.exe
    DWORD explorerPid = FindExplorerPid();
    if (!explorerPid) {
        Fail(L"Could not find explorer.exe. Is Windows Explorer running?");
        return 1;
    }

    // Build full path to ScreenCapture.dll (same folder as Veda.exe)
    wchar_t dllPath[32768] = {};
    DWORD len = GetModuleFileNameW(nullptr, dllPath, 32768);
    if (!len || len >= 32768) {
        Fail(L"Could not determine Veda installation path.");
        return 1;
    }
    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (!slash) { Fail(L"Invalid installation path."); return 1; }
    wcscpy(slash + 1, L"ScreenCapture.dll");

    // Inject DLL into explorer — Veda.exe exits after this returns
    if (!InjectDll(explorerPid, dllPath)) return 1;

    return 0; // Veda.exe closes completely — DLL lives inside explorer.exe
}
