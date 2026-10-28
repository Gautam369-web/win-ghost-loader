#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "LauncherSupport.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static int checks=0;
static void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
static DWORD ProbeChild(const std::wstring& name){
    wchar_t executable[32768]={};
    if(!GetModuleFileNameW(nullptr,executable,32768))throw std::runtime_error("test executable path");
    const std::wstring command=L"\""+std::wstring(executable)+L"\" --probe "+name;
    std::vector<wchar_t> writable(command.begin(),command.end());writable.push_back(0);
    STARTUPINFOW startup={};startup.cb=sizeof(startup);PROCESS_INFORMATION process={};
    if(!CreateProcessW(executable,writable.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,nullptr,&startup,&process))throw std::runtime_error("start lock test child");
    CloseHandle(process.hThread);
    const DWORD waited=WaitForSingleObject(process.hProcess,10000);
    DWORD code=999;const BOOL read=GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess);
    if(waited!=WAIT_OBJECT_0||!read)throw std::runtime_error("lock test child completion");
    return code;
}
int wmain(int argc,wchar_t** argv){
    if(argc==3&&wcscmp(argv[1],L"--probe")==0){
        veda::LaunchGuard guard(argv[2]);
        return !guard.valid()?3:guard.existing()?2:0;
    }
    try{
        const std::wstring name=L"Local\\VedaLauncherTest_"+std::to_wstring(GetCurrentProcessId());
        {
            veda::LaunchGuard first(name.c_str());
            Check(first.valid()&&!first.existing(),"first startup acquires lock");
            {veda::LaunchGuard second(name.c_str());Check(second.valid()&&second.existing(),"second startup blocked");}
            Check(ProbeChild(name)==2,"another process cannot start during startup");
            Check(ProbeChild(name)==2,"closing duplicate does not release original lock");
        }
        Check(ProbeChild(name)==0,"normal exit allows later launch");
        DWORD error=0;
        Check(veda::ModuleLoaded(GetCurrentProcessId(),L"kernel32.dll",error)==veda::ModuleState::loaded,"loaded module detected");
        Check(veda::ModuleLoaded(GetCurrentProcessId(),L"KERNEL32.DLL",error)==veda::ModuleState::loaded,"module names ignore case");
        Check(veda::ModuleLoaded(GetCurrentProcessId(),L"Veda_TEST_NOT_LOADED.dll",error)==veda::ModuleState::absent,"missing module allows startup");
        Check(veda::ModuleLoaded(0xffffffff,L"ScreenCapture.dll",error)==veda::ModuleState::unknown&&error!=0,"inspection failure blocks startup");
        std::cout<<"PASS: "<<checks<<" launcher checks; no Explorer injection, hooks, or network used.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
