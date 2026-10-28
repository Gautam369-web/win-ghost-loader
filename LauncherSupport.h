#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

namespace veda {
// Keep the handle alive throughout startup. Checking and immediately closing
// a named mutex allows two launchers to pass the same startup check.
class LaunchGuard {
    HANDLE handle_;
    DWORD error_;
public:
    explicit LaunchGuard(const wchar_t* name=L"Local\\VedaClientLauncher")
        : handle_(CreateMutexW(nullptr,FALSE,name)), error_(GetLastError()) {}
    ~LaunchGuard(){if(handle_)CloseHandle(handle_);}
    LaunchGuard(const LaunchGuard&)=delete;
    LaunchGuard& operator=(const LaunchGuard&)=delete;
    bool valid()const{return handle_!=nullptr;}
    bool existing()const{return valid()&&error_==ERROR_ALREADY_EXISTS;}
    DWORD error()const{return error_;}
};

enum class ModuleState { absent, loaded, unknown };
// Enumerating modules does not add a DLL load reference. Treat any copy with
// the same module name as running, including copies in other install folders.
inline ModuleState ModuleLoaded(DWORD pid,const wchar_t* name,DWORD& error){
    HANDLE snapshot=INVALID_HANDLE_VALUE;
    for(int attempt=0;attempt<3;++attempt){
        snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
        if(snapshot!=INVALID_HANDLE_VALUE)break;
        error=GetLastError();
        if(error!=ERROR_BAD_LENGTH)return ModuleState::unknown;
    }
    if(snapshot==INVALID_HANDLE_VALUE)return ModuleState::unknown;
    MODULEENTRY32W module={};module.dwSize=sizeof(module);
    BOOL found=Module32FirstW(snapshot,&module);
    while(found){
        if(_wcsicmp(module.szModule,name)==0){
            CloseHandle(snapshot);error=ERROR_SUCCESS;return ModuleState::loaded;
        }
        found=Module32NextW(snapshot,&module);
    }
    error=GetLastError();CloseHandle(snapshot);
    if(error!=ERROR_NO_MORE_FILES)return ModuleState::unknown;
    error=ERROR_SUCCESS;return ModuleState::absent;
}
}
