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

// ── run a kernel32 function with one wide-string arg in a remote process ─────

static bool RemoteCallW(HANDLE hProc, const char* fn, const wchar_t* arg) {
    LPVOID fnAddr = reinterpret_cast<LPVOID>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), fn));
    if (!fnAddr) return false;

    size_t argBytes = (wcslen(arg) + 1) * sizeof(wchar_t);
    LPVOID remArg = VirtualAllocEx(hProc, nullptr, argBytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remArg) return false;

    if (!WriteProcessMemory(hProc, remArg, arg, argBytes, nullptr)) {
        VirtualFreeEx(hProc, remArg, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hT = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(fnAddr), remArg, 0, nullptr);
    if (!hT) {
        VirtualFreeEx(hProc, remArg, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(hT, 8000);
    DWORD code = 0;
    GetExitCodeThread(hT, &code);
    CloseHandle(hT);
    VirtualFreeEx(hProc, remArg, 0, MEM_RELEASE);
    return code != 0;
}

// ── inject DLL into explorer ──────────────────────────────────────────────────

static bool InjectDll(DWORD pid, const wchar_t* dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        Fail(L"Cannot open explorer.exe.\nTry running Veda.exe as Administrator.", GetLastError());
        return false;
    }

    // Build our folder path (where ScreenCapture.dll + libwinpthread-1.dll live)
    wchar_t ourDir[32768] = {};
    wcscpy(ourDir, dllPath);
    wchar_t* slash = wcsrchr(ourDir, L'\\');
    if (slash) *slash = L'\0';

    // Step 1: Tell explorer to also search our folder for DLL dependencies
    //         → this lets explorer find libwinpthread-1.dll when loading ScreenCapture.dll
    RemoteCallW(hProc, "SetDllDirectoryW", ourDir);

    // Step 2: Load ScreenCapture.dll into explorer
    bool ok = RemoteCallW(hProc, "LoadLibraryW", dllPath);

    // Step 3: Reset DLL search path back to default
    RemoteCallW(hProc, "SetDllDirectoryW", L"");

    CloseHandle(hProc);

    if (!ok) {
        Fail(L"ScreenCapture.dll failed to load into explorer.exe.\n"
             L"Make sure ScreenCapture.dll and libwinpthread-1.dll are in the same folder as Veda.exe.");
        return false;
    }

    return true;
}

// ── check if already injected (mutex probe) ──────────────────────────────────

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
    wchar_t* sl = wcsrchr(dllPath, L'\\');
    if (!sl) { Fail(L"Invalid installation path."); return 1; }
    wcscpy(sl + 1, L"ScreenCapture.dll");

    // Inject DLL into explorer — Veda.exe exits after this returns
    if (!InjectDll(explorerPid, dllPath)) return 1;

    return 0; // Veda.exe closes completely — DLL lives inside explorer.exe
}
