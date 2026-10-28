#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include "LauncherSupport.h"

// ─── helpers ──────────────────────────────────────────────────────────────────

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
    MessageBoxW(nullptr, buf, L"NexoraX", MB_OK | MB_ICONERROR);
}

// ─── find explorer.exe PID ────────────────────────────────────────────────────

static DWORD FindExplorerPid() {
    DWORD pid = 0;
    // Use this desktop's shell, not an arbitrary folder window or another
    // signed-in user's Explorer process.
    HWND shell=GetShellWindow();
    if(shell)GetWindowThreadProcessId(shell,&pid);
    return pid;
}

// ─── run a kernel32 function with one wide-string arg in a remote process ─────

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

// ─── inject DLL into explorer ─────────────────────────────────────────────────

static bool InjectDll(DWORD pid, const wchar_t* dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        Fail(L"Cannot open explorer.exe.\nTry running NexoraX.exe as Administrator.", GetLastError());
        return false;
    }

    // Build our folder path (where WinHost.dll + libwinpthread-1.dll live)
    wchar_t ourDir[32768] = {};
    wcscpy(ourDir, dllPath);
    wchar_t* slash = wcsrchr(ourDir, L'\\');
    if (slash) *slash = L'\0';

    // Step 1: Tell explorer to also search our folder for DLL dependencies
    //         ↑ this lets explorer find libwinpthread-1.dll when loading WinHost.dll
    RemoteCallW(hProc, "SetDllDirectoryW", ourDir);

    // Step 2: Load WinHost.dll into explorer
    bool ok = RemoteCallW(hProc, "LoadLibraryW", dllPath);

    // Step 3: Reset DLL search path back to default
    RemoteCallW(hProc, "SetDllDirectoryW", L"");

    CloseHandle(hProc);

    if (!ok) {
        Fail(L"WinHost.dll failed to load into explorer.exe.\n"
             L"Make sure WinHost.dll and libwinpthread-1.dll are in the same folder as NexoraX.exe.");
        return false;
    }

    return true;
}

// ─── check if already injected (mutex probe) ──────────────────────────────────

static void ShowAlreadyRunning() {
    MessageBoxW(nullptr,L"NexoraX is already running or starting.\n"
        L"Press F7 to show its overlay.\n"
        L"To restart, press Alt+Q and wait for shutdown before opening NexoraX again.",
        L"NexoraX",MB_OK|MB_ICONINFORMATION);
}

// ─── entry point ──────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Single-instance check
    veda::LaunchGuard startup;
    if(!startup.valid()){
        Fail(L"Could not check whether NexoraX is already starting.",startup.error());
        return 1;
    }
    if (startup.existing()) {
        ShowAlreadyRunning();
        return 0;
    }

    // Find explorer.exe
    DWORD explorerPid = FindExplorerPid();
    if (!explorerPid) {
        Fail(L"Could not find explorer.exe. Is Windows Explorer running?");
        return 1;
    }
    DWORD moduleError=0;
    const auto state=veda::ModuleLoaded(explorerPid,L"WinHost.dll",moduleError);
    if(state==veda::ModuleState::loaded){ShowAlreadyRunning();return 0;}
    if(state==veda::ModuleState::unknown){
        Fail(L"Could not check Explorer for an existing NexoraX instance. Startup was stopped to avoid loading a duplicate.",moduleError);
        return 1;
    }

    // Build full path to WinHost.dll (same folder as NexoraX.exe)
    wchar_t dllPath[32768] = {};
    DWORD len = GetModuleFileNameW(nullptr, dllPath, 32768);
    if (!len || len >= 32768) {
        Fail(L"Could not determine NexoraX installation path.");
        return 1;
    }
    wchar_t* sl = wcsrchr(dllPath, L'\\');
    if (!sl) { Fail(L"Invalid installation path."); return 1; }
    wcscpy(sl + 1, L"WinHost.dll");

    // Inject DLL into explorer — NexoraX.exe exits after this returns
    if (!InjectDll(explorerPid, dllPath)) return 1;

    return 0; // NexoraX.exe closes completely — DLL lives inside explorer.exe
}
