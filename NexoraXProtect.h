/**
 * NexoraX Protection Layer
 * Compile-time string obfuscation + runtime anti-analysis guards.
 * Include ONCE at the top of ScreenCaptureDLL.cpp (before any headers).
 */
#pragma once

// ─────────────────────────────────────────────────────────────────
//  COMPILE-TIME XOR STRING OBFUSCATION
//  Usage:  const char* s = NX_STR("hello");   // decrypted at runtime
//          const wchar_t* w = NX_WSTR(L"hi"); // wide version
// ─────────────────────────────────────────────────────────────────

namespace nx_detail {

constexpr unsigned char XK = 0xA7; // XOR key — change to any nonzero byte

// Char array with XOR-encoded data, decrypts itself on first access
template<unsigned N>
struct ObfStr {
    char buf[N];
    constexpr ObfStr(const char (&s)[N]) : buf{} {
        for(unsigned i=0;i<N;++i) buf[i] = (char)(s[i] ^ (XK + i));
    }
    const char* get() const {
        // Decrypt in-place into a thread-local buffer (max 512 chars)
        static thread_local char dec[512];
        unsigned len = N < 512 ? N : 511;
        for(unsigned i=0;i<len;++i) dec[i]=(char)((unsigned char)buf[i]^(XK+i));
        dec[len]=0; return dec;
    }
};

template<unsigned N>
struct ObfWStr {
    wchar_t buf[N];
    constexpr ObfWStr(const wchar_t (&s)[N]) : buf{} {
        for(unsigned i=0;i<N;++i) buf[i]=(wchar_t)(s[i]^(XK+i));
    }
    const wchar_t* get() const {
        static thread_local wchar_t dec[512];
        unsigned len = N < 512 ? N : 511;
        for(unsigned i=0;i<len;++i) dec[i]=(wchar_t)((unsigned short)buf[i]^(XK+i));
        dec[len]=0; return dec;
    }
};

} // namespace nx_detail

// Macros — produce a temporary decrypted pointer
#define NX_STR(s)  ([]{ constexpr nx_detail::ObfStr<sizeof(s)> _o(s); return _o; }().get())
#define NX_WSTR(s) ([]{ constexpr nx_detail::ObfWStr<sizeof(s)/sizeof(wchar_t)> _o(s); return _o; }().get())


// ─────────────────────────────────────────────────────────────────
//  ANTI-DEBUG / ANTI-ANALYSIS GUARDS
//  Call NX_Guard() in DllMain (DLL_PROCESS_ATTACH) and on startup.
// ─────────────────────────────────────────────────────────────────

#include <windows.h>
#include <psapi.h>
#include <cstdint>

namespace nx_detail {

// 1. IsDebuggerPresent (PEB flag)
__forceinline bool DebuggerPresent() {
    return IsDebuggerPresent() != 0;
}

// 2. NtGlobalFlag check (PEB+0xBC set to 0x70 when debugger launched process)
__forceinline bool NtFlagSet() {
#ifdef _WIN64
    auto peb = reinterpret_cast<BYTE*>(__readgsqword(0x60));
    return *(DWORD*)(peb + 0xBC) == 0x70;
#else
    auto peb = reinterpret_cast<BYTE*>(__readfsdword(0x30));
    return *(DWORD*)(peb + 0x68) == 0x70;
#endif
}

// 3. HeapFlags check (debugger sets special heap flags)
__forceinline bool HeapFlagged() {
#ifdef _WIN64
    auto peb  = reinterpret_cast<BYTE*>(__readgsqword(0x60));
    auto heap = reinterpret_cast<BYTE*>(*(ULONG_PTR*)(peb + 0x30));
    return (*(DWORD*)(heap + 0x70) & 0x40) != 0;
#else
    auto peb  = reinterpret_cast<BYTE*>(__readfsdword(0x30));
    auto heap = reinterpret_cast<BYTE*>(*(ULONG_PTR*)(peb + 0x18));
    return (*(DWORD*)(heap + 0x44) & 0x40) != 0;
#endif
}

// 4. Timing check — debuggers slow down execution measurably
__forceinline bool TimingAnomaly() {
    LARGE_INTEGER t1, t2, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t1);
    // Trivial work
    volatile int x=0; for(int i=0;i<500;++i) x+=i;
    QueryPerformanceCounter(&t2);
    // >5ms for 500 iterations = definitely being stepped through
    double ms = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart;
    return ms > 5.0;
}

// 5. Check if known analysis tools are running
__forceinline bool AnalysisToolRunning() {
    const wchar_t* tools[] = {
        L"x64dbg.exe",   L"x32dbg.exe",  L"ollydbg.exe",
        L"windbg.exe",   L"ida.exe",      L"ida64.exe",
        L"idaq.exe",     L"idaq64.exe",   L"ghidra.exe",
        L"processhacker.exe", L"procmon.exe", L"wireshark.exe",
        L"fiddler.exe",  L"cheatengine.exe", L"scylla.exe",
        nullptr
    };
    DWORD pids[1024]; DWORD needed=0;
    if(!EnumProcesses(pids, sizeof(pids), &needed)) return false;
    DWORD count = needed/sizeof(DWORD);
    for(DWORD i=0;i<count;++i){
        HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pids[i]);
        if(!h) continue;
        wchar_t name[260]={};
        DWORD sz=260;
        QueryFullProcessImageNameW(h,0,name,&sz);
        CloseHandle(h);
        // Get just the filename part
        wchar_t* slash=wcsrchr(name,L'\\');
        const wchar_t* base = slash ? slash+1 : name;
        for(int j=0; tools[j]; ++j){
            if(_wcsicmp(base, tools[j])==0) return true;
        }
    }
    return false;
}

// Master guard — call on startup. If triggered: silent crash (no error box).
inline void NX_Guard() {
    bool bad = DebuggerPresent() || NtFlagSet() || HeapFlagged()
             || TimingAnomaly() || AnalysisToolRunning();
    if(bad){
        // Silent hard crash — no MessageBox, no trace
        *reinterpret_cast<volatile int*>(0) = 0; // null-deref → access violation
    }
}

} // namespace nx_detail

// Public alias
#define NX_Guard() nx_detail::NX_Guard()
