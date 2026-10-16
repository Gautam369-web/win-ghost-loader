#include <windows.h>
#include <wininet.h>
#include <algorithm>
#include <string>
#include <iostream>
static bool RequestCancelled(){return false;}
#include "HttpSupport.h"
#include "JsonSupport.h"
#include <fstream>
static std::string Utf8(const wchar_t* value){
    int size=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
    std::string out(size,'\0');WideCharToMultiByte(CP_UTF8,0,value,-1,out.data(),size,nullptr,nullptr);
    if(!out.empty())out.pop_back();return out;
}
static std::wstring Encode(const std::string& value){
    const wchar_t* digits=L"0123456789ABCDEF";std::wstring out;
    for(unsigned char c:value){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')out+=c;
        else{out+=L'%';out+=digits[c>>4];out+=digits[c&15];}}
    return out;
}
int main(int argc,char** argv){
    // Missing identity is deliberate: the endpoint validates it before any DB write.
    std::wstring url=L"https://vedax.vercel.app/api/verify";
    if(argc==2){
        std::ifstream file(argv[1]);sc::Json config;try{file>>config;}catch(...){return 2;}
        std::string name=config.value("name",std::string{}),reg=config.value("registration_number",std::string{});
        if(name.empty()||reg.empty())return 2;
        wchar_t user[128]={},computer[128]={},root[MAX_PATH]={};DWORD serial=0;
        GetEnvironmentVariableW(L"USERNAME",user,128);GetEnvironmentVariableW(L"COMPUTERNAME",computer,128);
        if(GetWindowsDirectoryW(root,MAX_PATH)){root[3]=0;GetVolumeInformationW(root,nullptr,0,&serial,nullptr,nullptr,nullptr,0);}
        url+=L"?hwid="+Encode(Utf8(user)+"_"+Utf8(computer))+L"&deviceId="+Encode(Utf8(computer)+"_"+std::to_string(serial))
            +L"&name="+Encode(name)+L"&reg="+Encode(reg);
    }
    auto result=HttpRequest(url,nullptr,20000);
    std::cout<<"GET verify (no identity): HTTP="<<result.status<<" Windows="<<result.windowsError
        <<" bodyBytes="<<result.body.size()<<"\n";
    auto data=sc::ParseJson(result.body);
    std::string status=sc::JsonString(result.body,"status");
    for(auto allowed:{"pending","approved","active","blocked","expired"})if(status==allowed)std::cout<<"Device status="<<allowed<<"\n";
    if(sc::JsonString(result.body,"error")=="Verification unavailable")std::cout<<"Server error=Verification unavailable\n";
    return result.windowsError==0&&(argc==2?result.status==200:result.status==400)?0:1;
}
