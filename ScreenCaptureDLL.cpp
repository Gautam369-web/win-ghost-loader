/*
 * ScreenCaptureDLL.cpp  --  All-in-one DLL with AI Answer + Stealth Typing
 *
 * STARTUP:  Reads user_config.json -> verifies HWID with vedax.vercel.app
 *           -> fetches Gemini API keys -> shows debug overlay
 *
 * HOTKEYS:
 *   Alt+S     -> Screenshot
 *   F8        -> Extract text under cursor -> send to Gemini AI
 *               -> show answer in floating notepad -> auto-load into buffer
 *   F9        -> Review MCQ suggestion; select the option manually
 *   Ctrl+Shift+C -> Copy the displayed answer
 *   Ctrl+F6   -> Load clipboard manually into buffer; typing is disabled
 *   F6        -> Toggle stealth ON/OFF  (per-key: each press types next buffer char)
 *   F6 x2     -> Reset buffer to start
 *   F12       -> Reset buffer index to 0
 *   F1/F2     -> Previous/next answer; typing is disabled on buffer change
 *   F7        -> Show/hide overlay; Shift+F8 -> Change subject
 *   Alt+Q     -> Stop, drain background workers, then release Explorer load
 *
 * user_config.json (place in same folder as ScreenCapture.dll):
 * {
 *   "registration_number": "21BCE1234",
 *   "name": "Your Name",
 *   "personal_api_key": "AIza...",   <- optional
 *   "default_language": "Python"
 * }
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wininet.h>
#include <ole2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <set>
#include <regex>
#include <algorithm>
#include <functional>
#include <sstream>
#include <fstream>
#include <cmath>
#include <atomic>
#include <mutex>
#include <memory>
#include "ScreenCaptureSupport.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

using namespace Gdiplus;

/* ============================================================
   GLOBALS
   ============================================================ */
static HMODULE       g_hModule      = NULL;
static HHOOK         g_hKbHook      = NULL;
static ULONG_PTR     g_gdiplusToken = 0;
static volatile LONG g_active       = 0;
static std::recursive_mutex g_stateMutex;
using StateLock = std::lock_guard<std::recursive_mutex>;
static std::atomic<bool> g_stopping{false};
static std::atomic<bool> g_configReady{false};
static std::atomic<int> g_extractGen{0};
static std::atomic<int> g_subjectIdx{0};
static std::atomic<DWORD> g_hookThreadId{0};
static std::mutex g_workersMutex, g_configMutex;
static std::vector<HANDLE> g_workers;
static thread_local int t_generation=-1, t_subject=0;
static thread_local HWND t_foreground=nullptr;
static thread_local POINT t_cursor={};
static bool RequestCancelled() {
    return g_stopping.load() || (t_generation>=0 && t_generation!=g_extractGen.load());
}
static void RequestStop() {
    g_stopping.store(true);
    ++g_extractGen;
    DWORD id=g_hookThreadId.load();
    if(id) PostThreadMessageW(id,WM_QUIT,0,0);
}
struct WorkerCall {
    LPTHREAD_START_ROUTINE fn;
    int generation, subject;
    HWND foreground;
    POINT cursor;
};
static DWORD WINAPI ManagedWorker(LPVOID value) {
    std::unique_ptr<WorkerCall> call(static_cast<WorkerCall*>(value));
    t_generation=call->generation; t_subject=call->subject;
    t_foreground=call->foreground; t_cursor=call->cursor;
    if(RequestCancelled()) return 0;
    try { return call->fn(nullptr); }
    catch(...) { OutputDebugStringW(L"ScreenCapture: background worker failed.\n"); return 1; }
}
static bool StartWorker(LPTHREAD_START_ROUTINE fn, bool extraction=false) {
    StateLock stateLock(g_stateMutex);
    std::lock_guard<std::mutex> lock(g_workersMutex);
    if(g_stopping.load()) return false;
    for(auto it=g_workers.begin(); it!=g_workers.end();) {
        if(WaitForSingleObject(*it,0)==WAIT_OBJECT_0) { CloseHandle(*it); it=g_workers.erase(it); }
        else ++it;
    }
    // Bound repeated hotkeys while providers or network requests are slow.
    if(g_workers.size()>=16) return false;
    g_workers.reserve(16);
    auto call=std::make_unique<WorkerCall>();
    call->fn=fn; call->generation=extraction ? ++g_extractGen : -1;
    call->subject=g_subjectIdx.load(); call->foreground=GetForegroundWindow();
    GetCursorPos(&call->cursor);
    HANDLE h=CreateThread(nullptr,0,ManagedWorker,call.get(),0,nullptr);
    if(!h) return false;
    call.release(); g_workers.push_back(h); return true;
}
static void JoinWorkers() {
    std::vector<HANDLE> workers;
    { std::lock_guard<std::mutex> lock(g_workersMutex); workers.swap(g_workers); }
    for(HANDLE h:workers) { WaitForSingleObject(h,INFINITE); CloseHandle(h); }
}

/* ============================================================
   PART A: SCREENSHOT ENGINE
   ============================================================ */
static std::wstring GetScreenshotPath() {
    wchar_t profile[MAX_PATH]={}; GetEnvironmentVariableW(L"USERPROFILE",profile,MAX_PATH);
    std::wstring dir=std::wstring(profile)+L"\\Pictures\\Screenshots\\";
    CreateDirectoryW((std::wstring(profile)+L"\\Pictures").c_str(),NULL); CreateDirectoryW(dir.c_str(),NULL);
    SYSTEMTIME st; GetLocalTime(&st); wchar_t name[64]={};
    swprintf_s(name,64,L"cap_%04d%02d%02d_%02d%02d%02d.png",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    return dir+name;
}
static bool SaveAsPng(HBITMAP hBmp,const std::wstring& path){
    if(!hBmp) return false; Bitmap bmp(hBmp,NULL); if(bmp.GetLastStatus()!=Ok) return false;
    UINT num=0,sz=0; GetImageEncodersSize(&num,&sz); if(!sz) return false;
    std::vector<BYTE> buf(sz); ImageCodecInfo* p=reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num,sz,p); CLSID clsid={};
    for(UINT i=0;i<num;i++) if(wcscmp(p[i].MimeType,L"image/png")==0){clsid=p[i].Clsid;break;}
    return (bmp.Save(path.c_str(),&clsid,NULL)==Ok);
}
static HBITMAP M0_BitBlt(){
    int w=GetSystemMetrics(SM_CXVIRTUALSCREEN),h=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    if(w<=0||h<=0) return NULL;
    HDC hSrc=GetDC(NULL),hMem=CreateCompatibleDC(hSrc); HBITMAP hBmp=CreateCompatibleBitmap(hSrc,w,h); SelectObject(hMem,hBmp);
    BOOL ok=BitBlt(hMem,0,0,w,h,hSrc,x,y,SRCCOPY|CAPTUREBLT); DeleteDC(hMem); ReleaseDC(NULL,hSrc);
    if(!ok){DeleteObject(hBmp);return NULL;} return hBmp;
}
static HBITMAP CaptureScreen(){ HBITMAP h=M0_BitBlt(); return h; }
static DWORD WINAPI ScreenshotThread(LPVOID){
    Sleep(200); if(RequestCancelled()) return 0; HBITMAP hBmp=CaptureScreen(); if(!hBmp) return 1;
    SaveAsPng(hBmp,GetScreenshotPath()); DeleteObject(hBmp); return 0;
}

/* ============================================================
   PART B: TEXT EXTRACTION ENGINE
   ============================================================ */
static std::string ToUtf8(const std::wstring& w){
    if(w.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,0); WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),s.data(),n,nullptr,nullptr); return s;
}
static std::wstring FromUtf8(const std::string& s){
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),NULL,0);
    std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n); return w;
}
static std::string TrimStr(const std::string& s){
    size_t start=s.compare(0,3,"\xef\xbb\xbf")==0?3:0;
    size_t a=s.find_first_not_of(" \t\r\n",start),b=s.find_last_not_of(" \t\r\n");
    return a==std::string::npos?std::string{}:s.substr(a,b-a+1);
}
#include "ClipboardSupport.h"
static bool SetClipboard(const std::string& text){
    if(RequestCancelled())return false;
    std::wstring value=FromUtf8(text);
    HWND owner=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,g_hModule,nullptr);
    if(!owner)return false;
    ClipboardApi api;
    bool ok=!RequestCancelled()&&PublishClipboardText(owner,value,api);
    DestroyWindow(owner);return ok;
}

static std::wstring GetDllDir(); // forward declaration

static std::wstring GetExtractBase(){
    // Use same folder as the running DLL/EXE
    std::wstring d = GetDllDir();
    return d.empty() ? L"C:\\VedaData\\" : d+L"VedaData\\";
}
// Ensure a subfolder exists and return its path
static std::wstring EnsureSubDir(const std::wstring& sub){
    std::wstring base=GetExtractBase();
    CreateDirectoryW(base.c_str(),NULL);
    std::wstring full=base+sub;  // base already ends with backslash
    CreateDirectoryW(full.c_str(),NULL);
    return full;
}
static std::wstring GetTimestampW(){
    SYSTEMTIME st; GetLocalTime(&st); wchar_t ts[32]={};
    swprintf_s(ts,32,L"%04d%02d%02d_%02d%02d%02d",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    return ts;
}
static bool WriteFile_(const std::wstring& path,const std::string& data){
    FILE* fp=_wfopen(path.c_str(),L"wb");
    if(!fp) return false;
    bool ok=fwrite(data.data(),1,data.size(),fp)==data.size();
    return fclose(fp)==0 && ok;
}
static bool WriteConfigAtomic(const std::wstring& path,const std::string& data){
    std::wstring temp=path+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId());
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD written=0;
    bool ok=WriteFile(file,data.data(),static_cast<DWORD>(data.size()),&written,nullptr)!=0 && written==data.size();
    if(ok) ok=FlushFileBuffers(file)!=0;
    if(!CloseHandle(file)) ok=false;
    if(ok) ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok) DeleteFileW(temp.c_str());
    return ok;
}

static std::vector<std::string> GetTextFromElement(IUIAutomationElement* el,std::set<std::string>& seen){
    std::vector<std::string> out; if(!el) return out;
    auto tryAdd=[&](BSTR b){
        if(!b) return; std::string s=TrimStr(ToUtf8(b)); SysFreeString(b);
        if(s.empty()||s=="Chrome Legacy Window"||s.find("Toggle Screen Reader")!=std::string::npos) return;
        if(seen.find(s)==seen.end()&&s.size()>1){seen.insert(s);out.push_back(s);}};
    BSTR name=nullptr; if(SUCCEEDED(el->get_CurrentName(&name))&&name) tryAdd(name);
    IUIAutomationValuePattern* vp=nullptr;
    if(SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,__uuidof(IUIAutomationValuePattern),(void**)&vp))&&vp){
        BSTR v=nullptr;if(SUCCEEDED(vp->get_CurrentValue(&v))&&v) tryAdd(v);vp->Release();}
    IUIAutomationTextPattern* tp=nullptr;
    if(SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId,__uuidof(IUIAutomationTextPattern),(void**)&tp))&&tp){
        IUIAutomationTextRange* tr=nullptr;
        if(SUCCEEDED(tp->get_DocumentRange(&tr))&&tr){BSTR t=nullptr;if(SUCCEEDED(tr->GetText(-1,&t))&&t) tryAdd(t);tr->Release();}
        tp->Release();}
    return out;
}
static void WalkChildren(IUIAutomationElement* parent,IUIAutomationTreeWalker* walker,
    const std::function<void(IUIAutomationElement*)>& cb){
    if(!parent||!walker) return;
    IUIAutomationElement* child=nullptr;
    if(FAILED(walker->GetFirstChildElement(parent,&child))||!child) return;
    while(child){if(RequestCancelled()){child->Release();break;}cb(child);IUIAutomationElement* next=nullptr;HRESULT hr=walker->GetNextSiblingElement(child,&next);child->Release();if(FAILED(hr)||!next) break;child=next;}
}
static std::string ExtractTextUnderCursor(HWND hwnd,int mx,int my,IUIAutomation* uia){
    if(!hwnd||!uia) return "";
    HWND target=hwnd;
    EnumChildWindows(hwnd,[](HWND child,LPARAM lp)->BOOL{
        wchar_t cls[256]={}; GetClassNameW(child,cls,256);
        if(wcsstr(cls,L"RenderWidget")||wcsstr(cls,L"LegacyWindow")||wcsstr(cls,L"Chrome_WidgetWin"))
            *reinterpret_cast<HWND*>(lp)=child; return TRUE;},reinterpret_cast<LPARAM>(&target));
    SendMessageTimeoutW(hwnd,WM_GETOBJECT,0,(LPARAM)0xFFFFFFFC,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,nullptr);
    if(target!=hwnd) SendMessageTimeoutW(target,WM_GETOBJECT,0,(LPARAM)0xFFFFFFFC,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,nullptr);
    Sleep(400);
    IUIAutomationTreeWalker* walker=nullptr;
    if(FAILED(uia->get_RawViewWalker(&walker))||!walker) uia->get_ControlViewWalker(&walker);
    std::set<std::string> seen; std::vector<std::string> output;
    POINT pt_hit={mx,my}; IUIAutomationElement* point_el=nullptr;
    if(SUCCEEDED(uia->ElementFromPoint(pt_hit,&point_el))&&point_el){
        auto t=GetTextFromElement(point_el,seen);output.insert(output.end(),t.begin(),t.end());
        if(walker){IUIAutomationElement* par=nullptr;
            if(SUCCEEDED(walker->GetParentElement(point_el,&par))&&par){
                auto up=GetTextFromElement(par,seen);output.insert(output.end(),up.begin(),up.end());par->Release();}}
        point_el->Release();}
    auto run_walk=[&](HWND h){
        IUIAutomationElement* r=nullptr;
        if(SUCCEEDED(uia->ElementFromHandle(h,&r))&&r){
            std::function<void(IUIAutomationElement*,int)> walk=[&](IUIAutomationElement* el,int depth){
                if(!el||depth>60||RequestCancelled()) return; RECT rect={};
                if(depth>0){if(FAILED(el->get_CurrentBoundingRectangle(&rect))) return;
                    if(rect.right>rect.left&&rect.bottom>rect.top)
                        if(!(rect.left<=mx&&mx<=rect.right&&rect.top<=my&&my<=rect.bottom)) return;}
                auto texts=GetTextFromElement(el,seen);output.insert(output.end(),texts.begin(),texts.end());
                if(walker) WalkChildren(el,walker,[&](IUIAutomationElement* c){walk(c,depth+1);});};
            walk(r,0);r->Release();}};
    run_walk(target);if(output.empty()&&target!=hwnd) run_walk(hwnd);
    if(walker) walker->Release();
    std::string result;for(auto& s:output) result+=s+"\n";
    if(!result.empty()&&result.back()=='\n') result.pop_back();
    return result;
}


/* ============================================================
   PART C: HTTP (WinInet) + JSON HELPERS
   ============================================================ */
static std::string UrlEncode(const std::string& s){
    std::string r; for(unsigned char c:s){
        if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') r+=(char)c;
        else{char buf[4];sprintf(buf,"%%%02X",c);r+=buf;}} return r;
}

#include "HttpSupport.h"
static std::string JStr(const std::string& text,const std::string& key){return sc::JsonString(text,key);}
static std::vector<std::string> JStrArr(const std::string& text,const std::string& key){
    auto j=sc::ParseJson(text);std::vector<std::string> out;
    if(!j.is_object()||!j.contains(key)||!j[key].is_array())return out;
    for(const auto& value:j[key]){if(!value.is_string())return {};out.push_back(value.get<std::string>());}
    return out;
}
static std::string GeminiExtractText(const std::string& text){return sc::GeminiText(text);}

/* ============================================================
   PART D: CONFIG + HWID + VERIFICATION + AI
   ============================================================ */
#define VEDA_HOST    L"vedax.vercel.app"
#define GEMINI_HOST  L"generativelanguage.googleapis.com"

static std::string  g_hwid;
static std::string  g_deviceId;
static std::string  g_regNum;
static std::string  g_userName;
static std::string  g_personalKey;
static std::atomic<bool> g_verified{false};
static char         g_verifyStatus[128] = "Verifying...";
static thread_local std::string t_debug;
static void PublishFailure(const char* message);

static std::wstring GetDllDir(){
    wchar_t path[MAX_PATH]={}; GetModuleFileNameW(g_hModule,path,MAX_PATH);
    wchar_t* last=wcsrchr(path,L'\\'); if(last) *(last+1)=0; return path;
}

static void MakeHwid(){
    wchar_t user[128]={},comp[128]={};
    DWORD sz=127; GetEnvironmentVariableW(L"USERNAME",user,sz); GetEnvironmentVariableW(L"COMPUTERNAME",comp,sz);
    g_hwid=ToUtf8(user)+"_"+ToUtf8(comp);
    wchar_t systemDir[MAX_PATH]={};
    DWORD serial=0;
    if(GetWindowsDirectoryW(systemDir,MAX_PATH)){
        systemDir[3]=0;
        GetVolumeInformationW(systemDir,nullptr,0,&serial,nullptr,nullptr,nullptr,0);
    }
    g_deviceId=ToUtf8(comp)+"_"+std::to_string(serial);
}

/* ============================================================
   SUBJECTS
   ============================================================ */
static const wchar_t* SUBJECTS[] = {
    L"Python", L"Java", L"C", L"C++",
    L"DBMS", L"Analytical", L"PEL (Communication Skill)", L"Others"
};
static const int SUBJECT_COUNT = 8;
// Subject is atomic; each extraction takes its own immutable snapshot.

static const wchar_t* CurrentSubject(){ return SUBJECTS[g_subjectIdx]; }
static const char*    CurrentSubjectA(){
    // Narrow-char subject name for AI prompts
    static const char* subs[]={"Python","Java","C","C++","DBMS","Analytical","PEL (Communication Skill)","Others"};
    if(g_subjectIdx>=0&&g_subjectIdx<8) return subs[g_subjectIdx];
    return "General";
}

/* ============================================================
   HACKER-THEMED SETUP DIALOG
   Dark terminal aesthetic — no title bar, neon green, scanlines
   ============================================================ */
// Dialog state
static std::wstring g_dlgName, g_dlgReg;
static int          g_dlgSubjIdx = 0;
static bool         g_dlgOk      = false;

#define ID_EDIT_NAME    301
#define ID_EDIT_REG     302
#define ID_CMB_SUBJ     303
#define ID_BTN_VERIFY   304

#define DLG_W    360
#define DLG_H    270
#define TITLE_H   36

// Glass palette — white/light theme
#define GL_BG       RGB(255, 255, 255)   // pure white (painted transparent via colorkey trick)
#define GL_PANEL    RGB(252, 253, 255)   // panel fill
#define GL_BORDER   RGB(210, 215, 230)   // subtle gray border
#define GL_TEXT     RGB(25,  30,  50 )   // near-black text
#define GL_LABEL    RGB(110, 115, 135)   // muted label
#define GL_ACCENT   RGB(60,  120, 255)   // blue accent
#define GL_EDIT     RGB(245, 247, 252)   // edit field bg
#define GL_BTN      RGB(55,  115, 250)   // button fill
#define GL_BTNHOV   RGB(35,   90, 220)   // button pressed
#define GL_CHROMA   RGB(6,    6,   6 )   // chroma key = transparent bg

static HFONT  g_glFont  = NULL;
static HFONT  g_glFontB = NULL;
static HFONT  g_glFontS = NULL;
static HBRUSH g_glEditBr= NULL;
static bool   g_glBlink = false;
static char   g_glStatus[128] = "Enter your details";

static void GlRect(HDC hdc,int x,int y,int w,int h,COLORREF c,int t=1){
    HPEN p=CreatePen(PS_SOLID,t,c);HPEN op=(HPEN)SelectObject(hdc,p);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,GetStockObject(NULL_BRUSH));
    Rectangle(hdc,x,y,x+w,y+h);
    SelectObject(hdc,op);SelectObject(hdc,ob);DeleteObject(p);
}
static void GlFill(HDC hdc,int x,int y,int w,int h,COLORREF c){
    RECT r={x,y,x+w,y+h};HBRUSH b=CreateSolidBrush(c);FillRect(hdc,&r,b);DeleteObject(b);
}

static LRESULT CALLBACK SetupWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){

    case WM_CREATE:{
        g_glFont =CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,VARIABLE_PITCH,L"Segoe UI");
        g_glFontB=CreateFontW(-13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,VARIABLE_PITCH,L"Segoe UI");
        g_glFontS=CreateFontW(-11,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,VARIABLE_PITCH,L"Segoe UI");
        g_glEditBr=CreateSolidBrush(GL_EDIT);
        strcpy(g_glStatus,"Enter your details");

        // Name edit
        HWND hN=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            16,60,DLG_W-32,24,hwnd,(HMENU)(UINT_PTR)ID_EDIT_NAME,g_hModule,NULL);
        SendMessageW(hN,WM_SETFONT,(WPARAM)g_glFont,TRUE);

        // Reg edit
        HWND hR=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_UPPERCASE,
            16,112,DLG_W-32,24,hwnd,(HMENU)(UINT_PTR)ID_EDIT_REG,g_hModule,NULL);
        SendMessageW(hR,WM_SETFONT,(WPARAM)g_glFont,TRUE);

        // Subject combo
        HWND hC=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            16,162,DLG_W-32,200,hwnd,(HMENU)(UINT_PTR)ID_CMB_SUBJ,g_hModule,NULL);
        SendMessageW(hC,WM_SETFONT,(WPARAM)g_glFont,TRUE);
        for(int i=0;i<SUBJECT_COUNT;i++) SendMessageW(hC,CB_ADDSTRING,0,(LPARAM)SUBJECTS[i]);
        SendMessageW(hC,CB_SETCURSEL,g_subjectIdx,0);

        // Verify button (owner-draw)
        CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            16,200,DLG_W-32,32,hwnd,(HMENU)(UINT_PTR)ID_BTN_VERIFY,g_hModule,NULL);

        SetTimer(hwnd,1,600,NULL);
        return 0;
    }

    case WM_TIMER:
        if(g_stopping.load()){DestroyWindow(hwnd);return 0;}
        g_glBlink=!g_glBlink;
        InvalidateRect(hwnd,NULL,FALSE);
        return 0;

    case WM_PAINT:{
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);

        // ── Chroma key bg (transparent area — desktop shows through) ──
        GlFill(hdc,0,0,rc.right,rc.bottom,GL_CHROMA);

        // ── Main glass card (white, slightly rounded feel with border) ──
        GlFill(hdc,4,4,rc.right-8,rc.bottom-8,GL_PANEL);

        // Outer border — very subtle gray
        GlRect(hdc,4,4,rc.right-8,rc.bottom-8,GL_BORDER,1);

        // Top accent line (blue stripe under title)
        GlFill(hdc,4,TITLE_H,rc.right-8,2,GL_ACCENT);

        SetBkMode(hdc,TRANSPARENT);
        HFONT oldF=(HFONT)SelectObject(hdc,g_glFontB);

        // ── Title ─────────────────────────────────────────────────────
        SetTextColor(hdc,GL_TEXT);
        RECT tRc={14,8,rc.right-40,TITLE_H};
        DrawTextW(hdc,L"Veda  Access",-1,&tRc,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        // Subtitle
        SelectObject(hdc,g_glFontS);
        SetTextColor(hdc,GL_LABEL);
        RECT stRc={14,20,rc.right-40,TITLE_H};
        DrawTextW(hdc,L"SECURE LOGIN",-1,&stRc,DT_LEFT|DT_BOTTOM|DT_SINGLELINE);

        // Close ×
        SelectObject(hdc,g_glFontB);
        SetTextColor(hdc,GL_LABEL);
        RECT xRc={rc.right-30,6,rc.right-6,TITLE_H-4};
        DrawTextW(hdc,L"×",-1,&xRc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        // ── Field labels ─────────────────────────────────────────────
        SelectObject(hdc,g_glFontS);
        SetTextColor(hdc,GL_LABEL);
        auto lbl=[&](int y,const wchar_t* t){RECT r={16,y,rc.right-16,y+14};DrawTextW(hdc,t,-1,&r,DT_LEFT|DT_SINGLELINE);};
        lbl(44, L"FULL NAME");
        lbl(96, L"REGISTRATION NUMBER");
        lbl(146,L"SUBJECT");

        // ── Edit field backgrounds + borders ─────────────────────────
        GlFill(hdc,16, 59,rc.right-32,26,GL_EDIT);
        GlRect(hdc,16, 59,rc.right-32,26,GL_BORDER,1);
        GlFill(hdc,16,111,rc.right-32,26,GL_EDIT);
        GlRect(hdc,16,111,rc.right-32,26,GL_BORDER,1);
        GlFill(hdc,16,161,rc.right-32,26,GL_EDIT);
        GlRect(hdc,16,161,rc.right-32,26,GL_BORDER,1);

        // ── Status bar ───────────────────────────────────────────────
        int sy=rc.bottom-30;
        GlFill(hdc,4,sy,rc.right-8,rc.bottom-4-sy,GL_PANEL);
        SelectObject(hdc,g_glFontS);
        SetTextColor(hdc,GL_LABEL);
        int n=MultiByteToWideChar(CP_ACP,0,g_glStatus,-1,NULL,0);
        std::wstring sw(n,0);MultiByteToWideChar(CP_ACP,0,g_glStatus,-1,&sw[0],n);
        sw+=(g_glBlink?L"|":L" ");
        RECT srRc={16,sy+2,rc.right-16,rc.bottom-6};
        DrawTextW(hdc,sw.c_str(),-1,&srRc,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        SelectObject(hdc,oldF);
        EndPaint(hwnd,&ps); return 0;
    }

    // ── Edit + combo styling ─────────────────────────────────────────
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:{
        HDC hdc=(HDC)wp;
        SetTextColor(hdc,GL_TEXT);
        SetBkColor(hdc,GL_EDIT);
        return (LRESULT)g_glEditBr;
    }

    // ── Owner-draw button ────────────────────────────────────────────
    case WM_DRAWITEM:{
        if(LOWORD(wp)!=ID_BTN_VERIFY) break;
        DRAWITEMSTRUCT* di=(DRAWITEMSTRUCT*)lp;
        HDC hdc=di->hDC; RECT r=di->rcItem;
        bool pressed=(di->itemState&ODS_SELECTED)!=0;
        COLORREF bc=pressed?GL_BTNHOV:GL_BTN;
        GlFill(hdc,r.left,r.top,r.right-r.left,r.bottom-r.top,bc);
        SetBkMode(hdc,TRANSPARENT);
        SetTextColor(hdc,RGB(255,255,255));
        HFONT of=(HFONT)SelectObject(hdc,g_glFontB);
        DrawTextW(hdc,L"Continue →",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(hdc,of);
        return TRUE;
    }

    // ── Drag by title bar ────────────────────────────────────────────
    case WM_NCHITTEST:{
        POINT pt={(short)LOWORD(lp),(short)HIWORD(lp)};
        ScreenToClient(hwnd,&pt);
        if(pt.y>=0&&pt.y<TITLE_H&&pt.x>=0&&pt.x<DLG_W-34) return HTCAPTION;
        return HTCLIENT;
    }

    // ── Close ×  ────────────────────────────────────────────────────
    case WM_LBUTTONDOWN:{
        int x=LOWORD(lp),y=HIWORD(lp);
        if(x>=DLG_W-32&&x<=DLG_W-6&&y>=4&&y<TITLE_H){DestroyWindow(hwnd);return 0;}
        return DefWindowProcW(hwnd,msg,wp,lp);
    }

    case WM_COMMAND:
        if(LOWORD(wp)==ID_BTN_VERIFY){
            wchar_t nb[256]={},rb[128]={};
            GetDlgItemTextW(hwnd,ID_EDIT_NAME,nb,255);
            GetDlgItemTextW(hwnd,ID_EDIT_REG, rb,127);
            std::wstring name=nb,reg=rb;
            auto trim=[](std::wstring& s){
                size_t a=s.find_first_not_of(L" \t\r\n"),b=s.find_last_not_of(L" \t\r\n");
                s=(a==std::wstring::npos)?L"":s.substr(a,b-a+1);};
            trim(name);trim(reg);
            if(name.empty()||reg.empty()){
                strcpy(g_glStatus,"Name and Registration are required");
                InvalidateRect(hwnd,NULL,FALSE);return 0;
            }
            g_dlgName=name;g_dlgReg=reg;
            g_dlgSubjIdx=(int)SendDlgItemMessageW(hwnd,ID_CMB_SUBJ,CB_GETCURSEL,0,0);
            if(g_dlgSubjIdx==CB_ERR) g_dlgSubjIdx=0;
            strcpy(g_glStatus,"Connecting...");
            InvalidateRect(hwnd,NULL,FALSE);UpdateWindow(hwnd);
            g_dlgOk=true; DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd,1);
        if(g_glFont){DeleteObject(g_glFont);g_glFont=NULL;}
        if(g_glFontB){DeleteObject(g_glFontB);g_glFontB=NULL;}
        if(g_glFontS){DeleteObject(g_glFontS);g_glFontS=NULL;}
        if(g_glEditBr){DeleteObject(g_glEditBr);g_glEditBr=NULL;}
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static void ShowSetupDialog(){
    if(g_stopping.load()) return;
    g_dlgOk=false;
    WNDCLASSEXW wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=SetupWndProc;
    wc.hInstance=g_hModule; wc.lpszClassName=L"SCSetup";
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    wc.hbrBackground=NULL;
    RegisterClassExW(&wc);
    int scrW=GetSystemMetrics(SM_CXSCREEN),scrH=GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_LAYERED,
        L"SCSetup",L"Veda",WS_POPUP,
        (scrW-DLG_W)/2,(scrH-DLG_H)/2,DLG_W,DLG_H,
        NULL,NULL,g_hModule,NULL);
    if(!hwnd){UnregisterClassW(L"SCSetup",g_hModule);return;}
    // Chroma key: GL_CHROMA pixels → transparent (desktop shows through)
    // Alpha 50 → 80% transparent on the non-chroma pixels
    SetLayeredWindowAttributes(hwnd, GL_CHROMA, 50, LWA_COLORKEY|LWA_ALPHA);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd); SetForegroundWindow(hwnd);
    MSG msg; while(GetMessageW(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(L"SCSetup",g_hModule);
}

static bool SaveUserConfig(const std::wstring& name,const std::wstring& reg,int subjIdx){
    if(subjIdx<0 || subjIdx>=SUBJECT_COUNT) return false;
    std::string key;
    { StateLock lock(g_stateMutex); key=g_personalKey; }
    std::wstring path=GetDllDir()+L"user_config.json";
    sc::Json config=sc::Json::object();
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES){
        std::ifstream file(path.c_str());if(!file)return false;
        std::string original((std::istreambuf_iterator<char>(file)),{});
        config=sc::ParseJson(original);if(!config.is_object())return false;
    }
    config["name"]=ToUtf8(name);config["registration_number"]=ToUtf8(reg);
    config["default_language"]=ToUtf8(SUBJECTS[subjIdx]);config["personal_api_key"]=key;
    return WriteConfigAtomic(path,config.dump(2)+"\n");
}

static void LoadUserConfig(){
    std::wstring path=GetDllDir()+L"user_config.json";
    std::ifstream f(path.c_str()); if(!f.is_open()) return;
    std::string json((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
    g_regNum     = JStr(json,"registration_number");
    g_userName   = JStr(json,"name");
    g_personalKey= JStr(json,"personal_api_key");
    std::string lang = JStr(json,"default_language");
    // Match subject index
    std::wstring langW=FromUtf8(lang);
    for(int i=0;i<SUBJECT_COUNT;i++){
        if(_wcsicmp(SUBJECTS[i],langW.c_str())==0){g_subjectIdx=i;break;}
        // partial match for PEL
        if(langW.size()>=3 && _wcsnicmp(SUBJECTS[i],L"PEL",3)==0 && _wcsnicmp(langW.c_str(),L"PEL",3)==0){g_subjectIdx=i;break;}
    }
}


static void SetVerifyStatus(const char* msg){
    StateLock lock(g_stateMutex);
    strncpy(g_verifyStatus,msg,127); g_verifyStatus[127]=0;
}
static void VerificationFailure(const char* stage,const HttpResult& response,ULONGLONG elapsed){
    std::string summary=std::string(stage)+": HTTP "+std::to_string(response.status)+", Windows "+std::to_string(response.windowsError);
    SetVerifyStatus(summary.c_str());
    t_debug=std::string(stage)+" | vedax.vercel.app\nHTTP "+std::to_string(response.status)
        +" | Windows "+std::to_string(response.windowsError)+" | "+std::to_string(elapsed)+" ms";
    const char* reason="Connection failed. Check Internet/proxy settings and the Windows error code below.";
    if(response.status==400)reason="Server rejected the identity fields. Check your saved name and registration details.";
    else if(response.status==401||response.status==403)reason="Server denied this device. Ask the admin to check device binding and access.";
    else if(response.status==404)reason="Server endpoint not found. Check the deployed dashboard version.";
    else if(response.status==429)reason="Verification is rate limited. Wait and restart Veda.";
    else if(response.status>=500)reason="Server failed verification/configuration. Admin: check deployment logs and database/settings.";
    else if(response.status>=300&&response.status<400)reason="Server redirected the request. Check the configured dashboard domain.";
    else if(response.windowsError==ERROR_INVALID_PARAMETER)reason="Windows rejected the HTTP request parameters. Use the updated client package.";
    PublishFailure(reason);
}

static void RunVerification(){
    if(RequestCancelled())return;
    if(g_regNum.empty()||g_userName.empty()){
        SetVerifyStatus("Setup required...");ShowSetupDialog();
        if(RequestCancelled()||!g_dlgOk){SetVerifyStatus("Setup cancelled.");return;}
        g_userName=ToUtf8(g_dlgName);g_regNum=ToUtf8(g_dlgReg);g_subjectIdx=g_dlgSubjIdx;
        if(!SaveUserConfig(g_dlgName,g_dlgReg,g_dlgSubjIdx)){SetVerifyStatus("Could not save configuration.");return;}
    }
    g_configReady=true;
    std::wstring query=L"?hwid="+FromUtf8(UrlEncode(g_hwid))+L"&deviceId="+FromUtf8(UrlEncode(g_deviceId))+L"&name="+FromUtf8(UrlEncode(g_userName))+L"&reg="+FromUtf8(UrlEncode(g_regNum));
    SetVerifyStatus("Checking access...");
    ULONGLONG started=GetTickCount64();
    auto verified=HttpRequest(std::wstring(L"https://")+VEDA_HOST+L"/api/verify"+query,nullptr,20000);
    if(RequestCancelled())return;
    if(!verified.ok()){
        // A transport failure is not an approval or an offline-access grant.
        VerificationFailure("Registration/verify",verified,GetTickCount64()-started);return;
    }
    std::string status=JStr(verified.body,"status");
    if(status!="active"&&status!="approved"){SetVerifyStatus("Access not approved; AI disabled.");return;}
    started=GetTickCount64();
    auto config=HttpRequest(std::wstring(L"https://")+VEDA_HOST+L"/api/config"+query,nullptr,20000);
    if(RequestCancelled())return;
    if(!config.ok()){VerificationFailure("API configuration",config,GetTickCount64()-started);return;}
    auto doc=sc::ParseJson(config.body);
    if(!doc.is_object()){SetVerifyStatus("Invalid API configuration.");return;}
    status=JStr(config.body,"status");
    if(doc.contains("status")&&status!="active"&&status!="approved"){
        SetVerifyStatus("API access not approved.");return;
    }
    if(!doc.contains("proxy")||!doc["proxy"].is_boolean()||!doc["proxy"].get<bool>()||!doc.contains("protocol")||doc["protocol"]!=2){
        SetVerifyStatus("Dashboard update required (protocol 2).");return;
    }
    if(RequestCancelled())return;
    g_verified=true;SetVerifyStatus("Access verified.");
}

// Forward declarations
static char g_lastLog[128];

// Shared data is protected by g_stateMutex. UI operations are posted to its owner.

static void SetLog(const char* msg);
static void SetAnswer(const std::wstring& answer,bool typeable=true); // forward declaration
static void SetProcessing(int step, bool s1done, bool s2done);
static void PublishFailure(const char* message); // forward declaration
extern std::atomic<int> g_overlayMode;     // defined in Part H
extern std::wstring  g_answerDisplay;   // defined in Part H
extern std::atomic<HWND> g_overlayHwnd;     // defined in Part H

// Helper macro: bail out silently if we've been superseded
#define CHECK_GEN(myGen) do{ if(RequestCancelled() || g_extractGen.load()!=(myGen)) return 0; }while(0)
static constexpr UINT WM_SC_SHOW=WM_APP+1, WM_SC_TOGGLE=WM_APP+2;
static void NotifyOverlay(bool toggle=false){
    HWND hwnd=g_overlayHwnd.load();
    if(hwnd && !g_stopping.load()) PostMessageW(hwnd,toggle?WM_SC_TOGGLE:WM_SC_SHOW,0,0);
}

// ── Subject-specific prompts (port of get_prompt_for_subject) ─────────────
static std::string GetPromptForSubject(int subjIdx, const std::string& text) {
    // Index: 0=Python 1=Java 2=C 3=C++ 4=DBMS 5=Analytical 6=PEL 7=Others
    switch(subjIdx) {
    case 0: // PYTHON
        return "You are a Python programming expert.\n\n"
               "EXTRACTED QUESTION / CODE:\n" + text + "\n\n"
               "TASK — follow exactly ONE of these two modes based on what you see above:\n\n"
               "MODE A — \"Fill the Gap\" (use this if the extracted text already contains Python code "
               "with a visible header and/or footer, e.g. import statements, function definitions, or placeholder comments):\n"
               "- Output ONLY the missing Python code that goes between the given header and footer.\n"
               "- DO NOT repeat any import statements, function signatures, or code already present.\n"
               "- Match the indentation level of the missing block.\n\n"
               "MODE B — \"Full Program\" (use this if the extracted text is a plain problem statement with NO existing Python code):\n"
               "- Output a complete, working Python program that solves the problem.\n"
               "- Include all necessary import statements at the top.\n"
               "- Use input() for input and print() for output.\n\n"
               "GLOBAL RULES (both modes):\n"
               "- DO NOT add any comments, explanations, or markdown.\n"
               "- DO NOT wrap output in code fences.\n"
               "- START immediately with the Python code. No preamble or closing remarks.\n"
               "- ZERO INDENTATION for top-level code: top-level lines must start at column 0, no leading spaces or tabs.";

    case 2: // C
        return "You are a C programming expert.\n\n"
               "EXTRACTED QUESTION / CODE:\n" + text + "\n\n"
               "TASK — follow exactly ONE of these two modes based on what you see above:\n\n"
               "MODE A — \"Fill the Gap\" (use this if the extracted text already contains C code "
               "with a visible header and/or footer, e.g. #include statements, a partial main function, or placeholder comments):\n"
               "- Output ONLY the missing C code that goes between the given header and footer.\n"
               "- DO NOT repeat any headers, include statements, function definitions, or signatures already present.\n\n"
               "MODE B — \"Full Program\" (use this if the extracted text is a plain problem statement with NO existing C code):\n"
               "- Output a complete, working C program that solves the problem.\n"
               "- Include necessary include statements at the top (e.g. #include <stdio.h>).\n"
               "- Use scanf/printf for input/output.\n"
               "- Include a proper int main() function.\n\n"
               "GLOBAL RULES (both modes):\n"
               "- DO NOT add any comments, explanations, or markdown.\n"
               "- DO NOT wrap output in code fences.\n"
               "- START immediately with the C code. No preamble or closing remarks.\n"
               "- ZERO INDENTATION: every line must start at column 0, no leading spaces or tabs.";

    case 3: // C++
        return "You are a C++ programming expert.\n\n"
               "EXTRACTED QUESTION / CODE:\n" + text + "\n\n"
               "TASK — follow exactly ONE of these two modes based on what you see above:\n\n"
               "MODE A — \"Fill the Gap\" (use this if the extracted text already contains C++ code "
               "with a visible header and/or footer, e.g. #include statements, using namespace std, or placeholder comments):\n"
               "- Output ONLY the missing C++ code that goes between the given header and footer.\n"
               "- DO NOT repeat any headers, include statements, or functions already present.\n\n"
               "MODE B — \"Full Program\" (use this if the extracted text is a plain problem statement with NO existing C++ code):\n"
               "- Output a complete, working C++ program that solves the problem.\n"
               "- Include necessary include statements at the top (e.g. #include <iostream>, using namespace std).\n"
               "- Use cin/cout for input/output.\n"
               "- Include a proper int main() function.\n\n"
               "GLOBAL RULES (both modes):\n"
               "- DO NOT add any comments, explanations, or markdown.\n"
               "- DO NOT wrap output in code fences.\n"
               "- START immediately with the C++ code. No preamble or closing remarks.\n"
               "- ZERO INDENTATION: every line must start at column 0, no leading spaces or tabs.";

    case 4: // DBMS
        return "You are a DBMS/SQL expert.\n\n"
               "EXTRACTED QUESTION / SCHEMA / PROBLEM:\n" + text + "\n\n"
               "TASK:\n"
               "- Output ONLY the correct SQL queries, relational algebra, or DBMS answers as requested by the problem.\n"
               "- DO NOT add any explanations, markdown, or code blocks.\n"
               "- START immediately with the SQL queries or answers. No preamble or comments.\n"
               "- ZERO INDENTATION: every line must start at column 0, no leading spaces or tabs.";

    case 1: // JAVA
        return "You are a Java programming expert.\n\n"
               "EXTRACTED QUESTION / CODE:\n" + text + "\n\n"
               "TASK — follow exactly ONE of these two modes based on what you see above:\n\n"
               "MODE A — \"Fill the Gap\" (use this if the extracted text already contains Java code "
               "with a visible header and/or footer, e.g. import statements, a partial class/method body, "
               "or placeholder comments like /* write your code here */):\n"
               "- Output ONLY the missing Java code that goes between the given header and footer.\n"
               "- DO NOT repeat any import statements, class declarations, or method signatures already present.\n"
               "- DO NOT include main() if it already appears in the footer.\n"
               "- ZERO INDENTATION: every line must start at column 0, no leading spaces or tabs.\n\n"
               "MODE B — \"Full Program\" (use this if the extracted text is a plain problem statement with NO existing Java code):\n"
               "- Output a complete, working Java program that solves the problem.\n"
               "- Include all necessary import statements at the top.\n"
               "- Include a proper public class with a main() method.\n"
               "- Use Scanner for input and System.out.println for output.\n"
               "- ZERO INDENTATION: every line must start at column 0, no leading spaces or tabs.\n\n"
               "GLOBAL RULES (both modes):\n"
               "- DO NOT add any comments, explanations, or markdown.\n"
               "- DO NOT wrap output in code fences.\n"
               "- START immediately with the Java code. No preamble or closing remarks.\n"
               "- Use correct Java syntax only (System.out.println/Scanner, not printf/scanf).";

    case 5: // ANALYTICAL
        return "You are an expert in analytical reasoning and aptitude.\n\n"
               "EXTRACTED QUESTION:\n" + text + "\n\n"
               "TASK:\n"
               "Solve the question. It may belong to any of these categories:\n"
               "- Logical Reasoning (syllogisms, analogies, statements & conclusions)\n"
               "- Verbal Reasoning (odd-one-out, series, classifications)\n"
               "- Numerical Aptitude (percentages, ratios, profit/loss, time-speed-distance, averages)\n"
               "- Number / Letter Series (find missing term)\n"
               "- Blood Relations\n"
               "- Direction Sense\n"
               "- Coding-Decoding\n"
               "- Data Interpretation (tables, bar charts, pie charts)\n"
               "- Seating Arrangements / Puzzles\n\n"
               "RULES:\n"
               "- Output ONLY the final answer or the correct option text.\n"
               "- If the answer is a single option letter (A/B/C/D), output it as: FINAL_ANSWER: [Letter] | [Exact option text]\n"
               "- If the answer is a value or word, output it as: FINAL_ANSWER: [value or word]\n"
               "- DO NOT add any working, steps, or explanations.\n"
               "- DO NOT use markdown or code fences.\n"
               "- START immediately with FINAL_ANSWER.";

    case 6: // PEL (Communication Skill)
        return "You are an expert in English communication skills and professional essay writing.\n\n"
               "EXTRACTED QUESTION / TOPIC / PROMPT:\n" + text + "\n\n"
               "TASK:\n"
               "Write an essay on the given topic/prompt adhering strictly to all of the following rules:\n"
               "- Write in short paragraphs so that the essay is more readable. Ensure that the sentences are simple and grammatically correct.\n"
               "- Please use proper punctuation marks while writing the essay.\n"
               "- Keep the introduction short and focus more on the topic and write points that are important and relevant to the topic.\n"
               "- Do not use bullet points. Do not write lengthy sentences. Keep your sentences short and simple.\n"
               "- Make the essay as descriptive as possible. However, please keep the essay within 200 words as going beyond the word limit would have a negative impact on your score.\n"
               "- Avoid using brand names.\n"
               "- Output ONLY the essay text itself. DO NOT add any title, greetings, markdown formatting, code fences, word count annotations, or concluding remarks.\n"
               "- START immediately with the first paragraph of the essay.";

    default: // Others
        return "You are a programming expert.\n\n"
               "EXTRACTED QUESTION / CODE:\n" + text + "\n\n"
               "TASK:\n"
               "- Solve the problem or answer the question.\n"
               "- Output ONLY the code or final answer.\n"
               "- DO NOT add any explanations, preamble, or markdown code fences.\n"
               "- START immediately with the solution.\n"
               "- ZERO INDENTATION: every line must start at column 0, no leading spaces or tabs.";
    }
}

// Cycle to next subject (Shift+F8)
static void CycleSubject(){
    if(!g_configReady.load() || RequestCancelled()) return;
    std::lock_guard<std::mutex> configLock(g_configMutex);
    if(RequestCancelled())return;
    int subject=(g_subjectIdx.load()+1)%SUBJECT_COUNT;
    if(!SaveUserConfig(FromUtf8(g_userName),FromUtf8(g_regNum),subject)){
        SetLog("Could not save subject; selection unchanged."); return;
    }
    g_subjectIdx.store(subject);
    char tmp[64]; sprintf(tmp,"Subject: %s",ToUtf8(SUBJECTS[subject]).c_str()); SetLog(tmp);
}


// ── Gemini model cascade ─────────────────────────────────────────────────────
struct GModel { const wchar_t* name; int timeoutMs; };
static const GModel GEMINI_MODELS[] = {
    {L"models/gemini-3.5-flash-lite", 25000},
    {L"models/gemini-3.1-flash-lite", 25000},
    {L"models/gemini-2.5-flash-lite", 25000},
    {L"models/gemini-2.5-flash", 25000},
};

// Clean code block markers from AI response
static std::string CleanAiResponse(const std::string& raw){ return sc::CleanResponse(raw); }

static std::string g_lastModel;
static int         g_lastTokens=0;

// Build JSON payload for Gemini
static std::string MakeGeminiPayload(const std::string& prompt,int maxTokens=4096){
    return sc::Json{{"contents",sc::Json::array({{{"parts",sc::Json::array({{{"text",prompt}}})}}})},
        {"generationConfig",{{"temperature",0.1},{"maxOutputTokens",maxTokens}}}}.dump();
}
static thread_local std::string t_aiError;
static thread_local bool t_mcqRequest=false;
static thread_local size_t t_extractedBytes=0;
static std::string SafeServiceError(const std::string& body){
    // Only our fixed public messages are displayed; never echo raw provider responses.
    std::string error=JStr(body,"error");
    const char* allowed[]={"AI is not ready. Ask the admin to test models and confirm project quotas.",
        "Verified capacity is busy or cooling down","Access denied","Too many requests",
        "Invalid request","Invalid model or token limit","AI request failed or timed out"};
    for(auto value:allowed)if(error==value)return error;
    return {};
}
static std::string CallGemini(const std::string& prompt){
    t_aiError="No complete AI answer received.";
    if(!g_verified.load()){
        StateLock lock(g_stateMutex);
        t_aiError="Access: "+std::string(g_verifyStatus)+" Stop and restart after approval.";return {};
    }
    // Veda selects a verified model with available project quota. No client retry storm.
    if(RequestCancelled())return {};
    std::wstring url=std::wstring(L"https://")+VEDA_HOST+L"/api/ai";
    std::string payload=sc::Json{{"hwid",g_hwid},{"deviceId",g_deviceId},{"endpoint",t_mcqRequest?"mcq":"code"},{"model","auto"},{"prompt",prompt},{"maxTokens",4096}}.dump();
    ULONGLONG started=GetTickCount64();
    auto response=HttpRequest(url,&payload,25000);
    t_debug+="\nHTTP "+std::to_string(response.status)+" | Windows "+std::to_string(response.windowsError)
        +" | "+std::to_string(GetTickCount64()-started)+" ms";
    if(RequestCancelled())return {};
    // Retry once on 503 (server overload) after 2 s
    if(response.status==503&&!RequestCancelled()){
        SetLog("MCQ: API busy (503). Retrying in 2s...");
        Sleep(2000);
        if(!RequestCancelled()){
            ULONGLONG started2=GetTickCount64();
            response=HttpRequest(url,&payload,25000);
            t_debug+="\nRETRY HTTP "+std::to_string(response.status)
                    +" | "+std::to_string(GetTickCount64()-started2)+" ms";
        }
    }
    if(!response.ok()){
        t_aiError=response.status==429?"AI capacity busy; wait before retrying.":response.error==HttpError::status?"AI service returned HTTP "+std::to_string(response.status):"AI request failed or timed out.";
        auto detail=SafeServiceError(response.body);
        if(!detail.empty())t_aiError=detail;
        return {};
    }
    std::string answer=CleanAiResponse(GeminiExtractText(response.body));
    if(answer.empty()){
        t_aiError="AI response was incomplete or invalid.";
        auto data=sc::ParseJson(response.body);
        if(data.is_object()&&data.contains("candidates")&&data["candidates"].is_array()&&!data["candidates"].empty()){
            auto& candidate=data["candidates"][0];
            if(candidate.is_object()&&candidate.value("finishReason",sc::Json{})=="MAX_TOKENS")
                t_aiError="AI hit its output token limit; shorten the question and retry.";
        }
    }
    return answer;
}

/* ============================================================
   PART E: PER-KEYPRESS STEALTH ENGINE
   ============================================================ */
#define STEALTH_MAGIC 0xC0FFEE42UL

static std::wstring    g_buf;
static volatile LONG   g_bufIdx    = 0;
static volatile LONG   g_stealthOn = 0;
static volatile LONG   g_suppVK    = 0;
static wchar_t         g_lastTyped = 0;
// g_lastLog declared earlier as forward declaration
static DWORD           g_lastF6    = 0;

static void SetLog(const char* msg){StateLock lock(g_stateMutex);if(RequestCancelled())return;strncpy(g_lastLog,msg,127);g_lastLog[127]=0;}

static bool InjectChar(wchar_t ch){
    INPUT inputs[2]={};
    WORD vk=ch==L'\n'?VK_RETURN:ch==L'\t'?VK_TAB:ch==L' '?VK_SPACE:0;
    for(int i=0;i<2;++i){
        inputs[i].type=INPUT_KEYBOARD;
        inputs[i].ki.dwExtraInfo=STEALTH_MAGIC;
        inputs[i].ki.wVk=vk;
        inputs[i].ki.wScan=vk?0:static_cast<WORD>(ch);
        inputs[i].ki.dwFlags=(vk?0:KEYEVENTF_UNICODE)|(i?KEYEVENTF_KEYUP:0);
    }
    if(SendInput(2,inputs,sizeof(INPUT))!=2) return false;
    g_lastTyped=ch; return true;
}

static void LoadIntoBuffer(const std::wstring& text){
    std::wstring normalized=sc::NormalizeNewlines(text);
    StateLock lock(g_stateMutex);
    if(RequestCancelled()) return;
    g_buf=std::move(normalized);
    g_bufIdx=0; g_suppVK=0; g_stealthOn=0;
}
static void LoadClipboard_(){
    std::wstring text;
    if(!OpenClipboard(NULL)) return;
    HANDLE data=GetClipboardData(CF_UNICODETEXT);
    if(data){
        const wchar_t* p=static_cast<const wchar_t*>(GlobalLock(data));
        if(p){
            size_t count=GlobalSize(data)/sizeof(wchar_t), end=0;
            while(end<count && p[end]) ++end;
            text.assign(p,end); GlobalUnlock(data);
        }
    }
    CloseClipboard();
    if(RequestCancelled()) return;
    LoadIntoBuffer(text);
    SetLog("Clipboard loaded; typing is OFF.");
}

static void HandleF6_(){
    StateLock lock(g_stateMutex);
    DWORD now=GetTickCount(); double elapsed=(now-g_lastF6)/1000.0;
    if(elapsed>=0.20&&elapsed<0.50){
        InterlockedExchange(&g_bufIdx,0);InterlockedExchange(&g_suppVK,0);
        SetLog("Buffer reset to start.");
    } else if(elapsed>=0.50){
        LONG wasOn=g_stealthOn;
        if(wasOn){InterlockedExchange(&g_stealthOn,0);InterlockedExchange(&g_suppVK,0);
            SetLog("Stealth OFF.");}
        else{if(!g_buf.empty()){InterlockedExchange(&g_stealthOn,1);
                char tmp[64];sprintf(tmp,"Stealth ON — pos %d/%d",(int)g_bufIdx,(int)g_buf.size());
                SetLog(tmp);}
            else{SetLog("No buffer! Use Ctrl+F6 or F8.");}}
    }
    g_lastF6=now;
}


/* ============================================================
   PART G: EXTRACTION + AI WORKER THREADS
   ============================================================ */
// ── File Savers ─────────────────────────────────────────────────────────────
// Folder layout next to the EXE:
//   VedaData/
//     extracted/   ← raw extracted text (one file per F8 press)
//     qa/          ← question + AI answer (one file per F8 press)
//     mcq/         ← MCQ options + AI answer (one file per F9 press)

static std::string MakeTimestamp(){
    SYSTEMTIME st; GetLocalTime(&st); char ts[32];
    sprintf(ts,"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    return ts;
}

// Save raw extracted text → VedaData/extracted/extracted_YYYYMMDD_HHMMSS.txt
static void SaveTextFile(const std::string& t){
    std::wstring dir = EnsureSubDir(L"extracted");
    std::wstring file = dir+L"\\extracted_"+GetTimestampW()+L".txt";
    WriteFile_(file, t);
}

// Save MCQ + answer → VedaData/mcq/mcq_YYYYMMDD_HHMMSS.txt
static void SaveMCQFile(const std::string& t){
    std::wstring dir = EnsureSubDir(L"mcq");
    std::wstring file = dir+L"\\mcq_"+GetTimestampW()+L".txt";
    WriteFile_(file, t);
}

// Save question + AI answer → VedaData/qa/qa_YYYYMMDD_HHMMSS.txt
static void SaveQA(const std::string& question, const std::string& answer, int subjIdx){
    std::wstring dir = EnsureSubDir(L"qa");
    std::wstring file = dir+L"\\qa_"+GetTimestampW()+L".txt";

    std::string subj = ToUtf8(SUBJECTS[subjIdx]);
    std::string ts   = MakeTimestamp();

    std::string entry;
    entry += "================================================================================\n";
    entry += "[" + ts + "]  Subject: " + subj + "\n";
    entry += "================================================================================\n\n";
    entry += "QUESTION:\n";
    entry += "--------------------------------------------------------------------------------\n";
    entry += question + "\n\n";
    entry += "ANSWER:\n";
    entry += "--------------------------------------------------------------------------------\n";
    entry += answer + "\n";

    WriteFile_(file, entry);
}


struct ComApartment {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~ComApartment(){if(SUCCEEDED(result))CoUninitialize();}
};
static std::string ExtractRequestText(){
    ComApartment apartment;
    if(FAILED(apartment.result))return {};
    IUIAutomation* raw=nullptr;
    if(FAILED(CoCreateInstance(__uuidof(CUIAutomation),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IUIAutomation),(void**)&raw))||!raw)return {};
    auto release=[](IUIAutomation* p){p->Release();};
    std::unique_ptr<IUIAutomation,decltype(release)> uia(raw,release);
    return ExtractTextUnderCursor(t_foreground,t_cursor.x,t_cursor.y,uia.get());
}
static DWORD WINAPI TextExtractThread(LPVOID){
    if(RequestCancelled())return 0;
    t_mcqRequest=false;t_debug="F8 | text extraction";t_extractedBytes=0;
    SetProcessing(1,false,false);
    std::string extracted=ExtractRequestText();
    t_extractedBytes=extracted.size();t_debug+="\nExtracted bytes: "+std::to_string(t_extractedBytes);
    if(RequestCancelled())return 0;
    if(extracted.empty()){PublishFailure("No accessible text. Point at the question, not the overlay, and retry.");return 0;}
    SetProcessing(2,true,false);
    std::string answer=CallGemini(GetPromptForSubject(t_subject,extracted));
    if(RequestCancelled())return 0;
    if(answer.empty()){PublishFailure(t_aiError.c_str());return 0;}
    SetAnswer(FromUtf8(answer));
    // Historical files may finish saving after supersession; UI state may not.
    if(!RequestCancelled()){SaveTextFile(extracted);SaveQA(extracted,answer,t_subject);}
    SetLog("Answer ready; press F6 to enable typing.");
    return 0;
}
// Micro-jitter fades as cursor approaches target.
// LCG randomizer -> no two moves identical -> bypasses bot detection.
static void MoveMouseHuman(int tx, int ty){
    POINT cur; GetCursorPos(&cur);
    int sx=cur.x, sy=cur.y;
    int scrW=GetSystemMetrics(SM_CXSCREEN);
    int scrH=GetSystemMetrics(SM_CYSCREEN);

    float dx=(float)(tx-sx), dy=(float)(ty-sy);
    float dist=sqrtf(dx*dx+dy*dy);
    if(dist<2.0f){ SetCursorPos(tx,ty); return; }

    // Fast LCG (no std::rand thread-safety issues)
    unsigned seed=(unsigned)GetTickCount()^((unsigned)sx*31337u^(unsigned)ty*7919u);
    auto rnd=[&](float lo,float hi)->float{
        seed=seed*1664525u+1013904223u;
        return lo+(((seed>>8)&0xFFFF)/65535.0f)*(hi-lo);
    };

    // Perpendicular unit vector for curve deviation
    float px=-dy/dist, py=dx/dist;

    // Cubic Bezier control points (two = cubic, more natural than quadratic)
    float dev1=rnd(-0.35f,0.35f)*dist;
    float dev2=rnd(-0.20f,0.20f)*dist;
    float cp1x=(float)sx+dx*0.25f+px*dev1, cp1y=(float)sy+dy*0.25f+py*dev1;
    float cp2x=(float)sx+dx*0.75f+px*dev2, cp2y=(float)sy+dy*0.75f+py*dev2;

    int steps=(int)(dist/11.0f)+12;
    if(steps<15) steps=15;
    if(steps>55) steps=55;

    for(int i=1;i<=steps;i++){
        if(RequestCancelled()) return;
        float t=(float)i/(float)steps;
        float e=t*t*(3.0f-2.0f*t); // smoothstep
        float u=1.0f-e;

        float x=u*u*u*(float)sx +3*u*u*e*cp1x +3*u*e*e*cp2x +e*e*e*(float)tx;
        float y=u*u*u*(float)sy +3*u*u*e*cp1y +3*u*e*e*cp2y +e*e*e*(float)ty;

        float jf=(1.0f-t)*1.6f;
        x+=rnd(-1.0f,1.0f)*jf;
        y+=rnd(-1.0f,1.0f)*jf;

        x=std::max(0.0f,std::min((float)(scrW-1),x));
        y=std::max(0.0f,std::min((float)(scrH-1),y));

        SetCursorPos((int)x,(int)y);

        float spd=sinf(t*3.14159f);
        int ms=(int)(14.0f-10.0f*spd)+(int)rnd(0.0f,4.0f);
        Sleep(ms);
    }
    SetCursorPos(tx,ty);
}

// Z-pattern corners: A=top-left  B=top-right  C=bot-left  D=bot-right
static void MoveMCQMouse(char letter){
    if(letter<'A'||letter>'E') return;
    int scrW=GetSystemMetrics(SM_CXSCREEN), scrH=GetSystemMetrics(SM_CYSCREEN);
    int cx, cy;
    switch(letter){
        case 'A': cx=80;        cy=80;        break; // top-left
        case 'B': cx=scrW-80;   cy=80;        break; // top-right
        case 'C': cx=80;        cy=scrH-80;   break; // bottom-left
        case 'D': cx=scrW-80;   cy=scrH-80;   break; // bottom-right
        default:  cx=scrW/2;    cy=scrH/2;    break; // E: center screen
    }
    MoveMouseHuman(cx, cy);
}

static char ParseMCQLetter(const std::string& text){return sc::McqLetter(text);}

// MCQ result state (protected by g_stateMutex)
static char         g_mcqLetter   = 0;     // 0 = not MCQ result; 'A'-'D' when done
static std::wstring g_mcqQuestion;          // truncated question snippet for display
static std::wstring g_mcqCorner;            // e.g. L"TOP-LEFT"

// ── CleanMCQText: strip noise and hard-cap before sending to AI ──────────────
// MCQ questions are short (<600 chars usually). Large dumps = page garbage.
// We keep at most MCQ_MAX_CHARS chars; we also collapse runs of whitespace.
static const size_t MCQ_MAX_CHARS = 1500;
static std::string CleanMCQText(const std::string& raw){
    std::string out;
    out.reserve(raw.size());
    bool lastWasSpace=false;
    for(size_t i=0;i<raw.size()&&out.size()<MCQ_MAX_CHARS;i++){
        unsigned char c=(unsigned char)raw[i];
        if(c=='\r') continue;                    // strip carriage returns
        if(c=='\t') c=' ';                       // tab -> space
        if(c==' '||c=='\n'){
            // collapse multiple blanks/newlines but keep single newlines
            if(!lastWasSpace){out+=(char)c;lastWasSpace=true;}
        } else {
            out+=(char)c;lastWasSpace=false;
        }
    }
    // trim trailing whitespace
    while(!out.empty()&&(out.back()==' '||out.back()=='\n')) out.pop_back();
    return out;
}

static std::wstring g_mcqOptions[5];    // parsed option texts A-E (protected by g_stateMutex)
static int          g_mcqOptCount = 4;  // 4 or 5 options found
static int          g_mcqScrollY  = 0;  // scroll offset for MCQ overlay (pixels)
static int          g_ansScrollY  = 0;  // scroll offset for code answer overlay

// ── ParseMCQOptions: extract option text for A-E from MCQ text ─────────────
// Recognises: "A) text"  "A. text"  "(A) text"  "(A.) text"
// Does NOT match "A:" to avoid false positives in code/prose
static int ParseMCQOptions(const std::string& text){
    for(int i=0;i<5;i++) g_mcqOptions[i].clear();
    int found=0;
    // split into lines
    size_t pos=0;
    while(pos<=text.size()){
        size_t nl=text.find('\n',pos);
        std::string ln=(nl==std::string::npos)?text.substr(pos):text.substr(pos,nl-pos);
        pos=(nl==std::string::npos)?text.size()+1:nl+1;
        // strip \r and leading spaces
        if(!ln.empty()&&ln.back()=='\r') ln.pop_back();
        size_t s=0; while(s<ln.size()&&(ln[s]==' '||ln[s]=='\t')) s++;
        if(ln.size()-s<3) continue;
        ln=ln.substr(s);
        int idx=-1;
        std::string optText;
        char ch0=ln[0], ch1=ln.size()>1?ln[1]:0;
        // Pattern 1: "A) text"  or "A. text"
        if(ch0>='A'&&ch0<='E'&&(ch1==')' ||ch1=='.')&&ln.size()>2){
            idx=ch0-'A';
            optText=ln.substr(2);
        }
        // Pattern 2: "(A) text"
        else if(ch0=='('&&ln.size()>3&&ln[1]>='A'&&ln[1]<='E'&&ln[2]==')'){
            idx=ln[1]-'A';
            optText=ln.substr(3);
        }
        // Pattern 3: "(A.) text"
        else if(ch0=='('&&ln.size()>4&&ln[1]>='A'&&ln[1]<='E'&&ln[2]=='.'&&ln[3]==')'){
            idx=ln[1]-'A';
            optText=ln.substr(4);
        }
        if(idx<0||idx>4) continue;
        // trim leading space from option text
        size_t ts=0; while(ts<optText.size()&&optText[ts]==' ') ts++;
        optText=optText.substr(ts);
        // Reject lines that look like code (contain { } ; = ->)
        bool looksLikeCode=false;
        if(optText.find("->")!=std::string::npos||optText.find("{")!=std::string::npos
           ||optText.find("};")!=std::string::npos||optText.find("std::")!=std::string::npos)
            looksLikeCode=true;
        if(looksLikeCode) continue;
        if(!optText.empty()&&(int)g_mcqOptions[idx].empty()){
            g_mcqOptions[idx]=FromUtf8(optText);
            if(idx+1>found) found=idx+1;
        }
    }
    return (found<4)?4:(found>5?5:found);
}

static DWORD WINAPI MCQExtractThread(LPVOID){
    if(RequestCancelled())return 0;
    t_mcqRequest=true;
    t_debug="F9 | MCQ extraction"; t_extractedBytes=0;

    // Clear any previous MCQ result globals before starting
    { StateLock lk(g_stateMutex);
      g_mcqLetter=0; g_mcqQuestion.clear(); g_mcqCorner.clear();
      for(int _i=0;_i<5;_i++) g_mcqOptions[_i].clear();
      g_mcqOptCount=4; g_mcqScrollY=0; }

    // ── Step 1: Reading question text ─────────────────────────────
    SetLog("MCQ: Reading question text from screen...");
    SetProcessing(3,false,false);
    std::string question=ExtractRequestText();
    t_extractedBytes=question.size();          // raw byte count (before cleaning)
    t_debug+="\nExtracted bytes: "+std::to_string(t_extractedBytes);
    // Strip noise & hard-cap to MCQ_MAX_CHARS before sending to AI
    bool wasTrimmed=(question.size()>MCQ_MAX_CHARS);
    question=CleanMCQText(question);

    if(RequestCancelled())return 0;
    if(question.empty()){
        PublishFailure("No accessible question text. Point at the question and options, then retry.");
        return 0;
    }

    // Store Q: snippet — skip "QUESTION:" prefix and short label lines
    { std::string snipSrc = question;
      // Strip leading junk tokens: "QUESTION:", "Question:", numbers, short words
      // Find first line that looks like the actual question (has letters+punctuation, >=20 chars)
      // or if none found, use the whole thing from char 0
      size_t skipTo = 0;
      {
        size_t si = 0;
        while(si < snipSrc.size()){
            size_t nl = snipSrc.find('\n', si);
            size_t len = (nl==std::string::npos) ? snipSrc.size()-si : nl-si;
            std::string ln = snipSrc.substr(si, len);
            // Strip "QUESTION:" prefix token if present
            if(ln.size()>=8 && ln.substr(0,8)=="QUESTION"){ si=(nl==std::string::npos)?snipSrc.size():nl+1; continue; }
            if(ln.size()>=8 && ln.substr(0,8)=="Question"){ si=(nl==std::string::npos)?snipSrc.size():nl+1; continue; }
            // Skip very short lines (app title, labels < 5 chars)
            if(ln.size()<5){ si=(nl==std::string::npos)?snipSrc.size():nl+1; continue; }
            // Found a real line
            skipTo=si; break;
        }
      }
      std::string snippet = snipSrc.substr(skipTo, std::min((size_t)90, snipSrc.size()-skipTo));
      for(char& c:snippet) if(c=='\n'||c=='\r'||c=='\t') c=' ';
      // Collapse multiple spaces
      std::string snippetClean; bool sp2=false;
      for(char c:snippet){if(c==' '){if(!sp2){snippetClean+=c;sp2=true;}}else{snippetClean+=c;sp2=false;}}
      StateLock lk(g_stateMutex);
      g_mcqQuestion = FromUtf8(snippetClean);
    }
    // ── Parse option lines from cleaned text ─────────────────────────
    { StateLock lk(g_stateMutex);
      g_mcqOptCount = ParseMCQOptions(question);
      g_mcqScrollY  = 0; }
    SetLog(("MCQ: "+std::to_string(t_extractedBytes)+" bytes raw"
           +(wasTrimmed?" \u2192 trimmed to "+std::to_string(question.size())+" chars":"")+". Analyzing...").c_str());

    // ── Step 2: Asking Gemini AI ────────────────────────────────────
    SetProcessing(3,true,false);
    SetLog("MCQ: Sending to Gemini AI for analysis...");

    std::string prompt=
        "You are solving a multiple-choice question (MCQ).\n"
        "Read the question and ALL options carefully, then identify the correct answer.\n"
        "If the question or its options are missing or too ambiguous, reply exactly: UNKNOWN\n"
        "Otherwise reply in this EXACT format (nothing else):\n"
        "LETTER|exact option text\n"
        "Example: D|Push  or  B|Quick Sort\n"
        "The LETTER must be A, B, C, D, or E.\n"
        "The option text must be the exact wording of the correct option.\n\n"
        "Question and options:\n"+question;
    std::string answer=CallGemini(prompt);

    if(RequestCancelled())return 0;
    if(answer.empty()){PublishFailure(t_aiError.c_str());return 0;}

    // Parse LETTER|OptionText response (or fall back to single-letter)
    char letter=0;
    std::string aiOptionText;
    {
        std::string trimmed=TrimStr(answer);
        // Try "X|text" format first
        if(trimmed.size()>=3 && trimmed[1]=='|' &&
           trimmed[0]>='A' && trimmed[0]<='E'){
            letter=trimmed[0];
            aiOptionText=TrimStr(trimmed.substr(2));
        } else {
            // Fallback: try single letter
            letter=ParseMCQLetter(trimmed);
        }
    }
    // If AI gave us the option text and we don't have it from parsing yet, store it
    if(letter && !aiOptionText.empty()){
        StateLock lk(g_stateMutex);
        int li=letter-'A';
        if(li>=0&&li<5&&g_mcqOptions[li].empty())
            g_mcqOptions[li]=FromUtf8(aiOptionText);
    }
    if(!letter){
        PublishFailure(TrimStr(answer)=="UNKNOWN"?
            "AI returned UNKNOWN: question or options missing or ambiguous. Point at the full question and retry.":
            "AI returned an unexpected MCQ format (expected A-D). No option selected. Retry the full question.");
        return 0;
    }

    // ── Step 3: Prepare result and show overlay ─────────────────────────
    // Z-pattern corners: A=top-left B=top-right C=bottom-left D=bottom-right
    static const wchar_t* CORNERS[]={L"TOP-LEFT",L"TOP-RIGHT",L"BOTTOM-LEFT",L"BOTTOM-RIGHT",L"CENTER"};
    int ci = letter-'A';
    std::wstring cornerW = (ci>=0&&ci<5) ? CORNERS[ci] : L"UNKNOWN";

    { StateLock lk(g_stateMutex);
      g_mcqLetter = letter;
      g_mcqCorner = cornerW;
    }
    SetLog(("MCQ: AI answered option "+std::string(1,letter)
           +" ("+std::string(cornerW.begin(),cornerW.end())+"). Displaying...").c_str());

    std::wstring display = std::wstring(L"MCQ Answer: ") + (wchar_t)letter
                         + L"\n\nMoving mouse to: " + cornerW + L" corner";
    SetAnswer(display,false);

    if(!RequestCancelled())
        SaveMCQFile("QUESTION:\n"+question+"\n\nAnswer: "+letter
                   +"\nCorner: "+std::string(cornerW.begin(),cornerW.end()));

    SetLog(("MCQ option "+std::string(1,letter)+" shown. Mouse moving to "
           +std::string(cornerW.begin(),cornerW.end())+".").c_str());
    MoveMCQMouse(letter);
    return 0;
}


struct AnswerRecord {std::wstring text;int subject;bool typeable;};
static std::vector<AnswerRecord> g_answerHistory;
static int g_displaySubject=0;
static int                       g_histIdx = -1; // current position in history

std::atomic<HWND> g_overlayHwnd{nullptr};
std::atomic<int> g_overlayMode{0};       // 0=shortcuts, 1=answer, 2=processing
std::wstring g_answerDisplay;           // last AI answer (for display)

// Mode 2 — Processing state
// steps: 0=idle  1=extracting  2=asking AI  3=MCQ scanning
static int  g_procStep  = 0;
static bool g_step1Done = false;
static bool g_step2Done = false;
static int  g_spinTick  = 0;  // increments each 250ms timer for spinner

// Helper: switch to processing mode and auto-show overlay
static void SetProcessing(int step, bool s1done, bool s2done){
    { StateLock stateLock(g_stateMutex);
    if(RequestCancelled()) return;
    g_procStep  = step;
    g_step1Done = s1done;
    g_step2Done = s2done;
    g_overlayMode = 2;
    }
    NotifyOverlay();
}

#define OVL_W  330
#define OVL_H  240

// Called from TextExtractThread when AI answer is ready
static void SetAnswer(const std::wstring& answer,bool typeable){
    { StateLock stateLock(g_stateMutex);
    if(RequestCancelled()) return;
    // Add to history (truncate forward history if we went back)
    if(g_histIdx >= 0 && g_histIdx < (int)g_answerHistory.size()-1)
        g_answerHistory.erase(g_answerHistory.begin()+g_histIdx+1, g_answerHistory.end());
    g_answerHistory.push_back({answer,t_subject,typeable});
    if(g_answerHistory.size() > 50) g_answerHistory.erase(g_answerHistory.begin());
    g_histIdx = (int)g_answerHistory.size()-1;
    g_answerDisplay = answer;
    g_displaySubject=t_subject;
    if(typeable){g_mcqLetter=0;for(int _i=0;_i<5;_i++) g_mcqOptions[_i].clear();g_mcqOptCount=4;g_mcqScrollY=0;}  // clear MCQ state
    g_ansScrollY=0;  // reset answer scroll on new result
    LoadIntoBuffer(typeable?answer:std::wstring{});
    g_overlayMode   = 1;
    }
    // Auto-show overlay (even if hidden by F7)
    NotifyOverlay();
}

// ── Helper to convert narrow string → wstring for overlay display
static std::wstring AtoW(const char* s){return s?FromUtf8(s):std::wstring{};}
static void PublishFailure(const char* message){
    {StateLock lock(g_stateMutex);if(RequestCancelled())return;
    g_overlayMode=3;g_stealthOn=0;g_procStep=0;
    g_answerDisplay=FromUtf8(std::string(message)+"\n\n"+t_debug);
    LoadIntoBuffer(L"");SetLog("Request failed | F7 hide/show | Ctrl+Shift+C copy debug");}
    NotifyOverlay();
}
struct OverlaySnapshot {
    int mode,subject,step,spin;LONG index;size_t total;bool typing,verified,step1,step2;
    std::wstring answer;std::string log,status;
    char         mcqLetter;       // 0 or 'A'-'D'
    std::wstring mcqQuestion;     // truncated question snippet
    std::wstring mcqCorner;       // e.g. L"TOP-LEFT"
    std::wstring mcqOptions[5]; // option texts A-E
    int          mcqOptCount;   // 4 or 5
    int          mcqScrollY;    // scroll offset
    int          ansScrollY;    // scroll offset for code answer
};
static OverlaySnapshot SnapshotOverlay(){
    StateLock lock(g_stateMutex);
    OverlaySnapshot s;
    s.mode     = g_overlayMode.load();
    s.subject  = (s.mode==1) ? g_displaySubject : g_subjectIdx.load();
    s.step     = g_procStep;
    s.spin     = g_spinTick;
    s.index    = g_bufIdx;
    s.total    = g_buf.size();
    s.typing   = (g_stealthOn != 0);
    s.verified = g_verified.load();
    s.step1    = g_step1Done;
    s.step2    = g_step2Done;
    s.answer   = g_answerDisplay;
    s.log      = g_lastLog;
    s.status   = g_verifyStatus;
    s.mcqLetter   = g_mcqLetter;
    s.mcqQuestion = g_mcqQuestion;
    s.mcqCorner   = g_mcqCorner;
    for(int _i=0;_i<5;_i++) s.mcqOptions[_i]=g_mcqOptions[_i];
    s.mcqOptCount = g_mcqOptCount;
    s.mcqScrollY  = g_mcqScrollY;
    s.ansScrollY  = g_ansScrollY;
    return s;
}


static void DrawProcessing(HDC hdc, const RECT& rc,const OverlaySnapshot& view){
    const wchar_t* spinner[]={L"⠋",L"⠙",L"⠹",L"⠸",L"⠼",L"⠴",L"⠦",L"⠧",L"⠇",L"⠏"};
    // Fallback ASCII spinner if Braille glyphs don't render
    const wchar_t* asp[]={L"|",L"/",L"—",L"\\"};
    const wchar_t* sp = asp[view.spin % 4];

    bool isMCQ = (view.step == 3);

    // ── Title ──────────────────────────────────────────────────────────────
    HFONT fntB=CreateFontW(-13,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    HFONT of=(HFONT)SelectObject(hdc,fntB);
    SetTextColor(hdc, RGB(120,80,0));
    RECT tRc={6,4,rc.right-6,20};
    wchar_t title[80]={};
    swprintf_s(title,80,L"  %s  Processing...  [%s]  %s",
        sp, SUBJECTS[view.subject], isMCQ?L"MCQ":L"F8");
    DrawTextW(hdc,title,-1,&tRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
    SelectObject(hdc,of); DeleteObject(fntB);

    // Separator
    auto hline=[&](int y,COLORREF c){
        HPEN p=CreatePen(PS_SOLID,1,c);HPEN op=(HPEN)SelectObject(hdc,p);
        MoveToEx(hdc,4,y,NULL);LineTo(hdc,rc.right-4,y);
        SelectObject(hdc,op);DeleteObject(p);};
    hline(22,RGB(80,70,20));

    HFONT fnt=CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    HFONT ofc=(HFONT)SelectObject(hdc,fnt);

    // ── Step rows ──────────────────────────────────────────────────────────
    // Each step: icon + description + state badge
    struct Step {
        const wchar_t* desc;
        bool done;
        bool active;
    };
    Step steps[3];

    if(!isMCQ){
        steps[0] = {L"Extracting text from screen",  view.step1, (!view.step1 && view.step>=1)};
        steps[1] = {L"Sending to Gemini AI...",       view.step2, (!view.step2 && view.step>=2)};
        steps[2] = {L"Loading answer into buffer",    (view.step==0&&view.step2), false};
    } else {
        steps[0] = {L"Reading question text",  view.step1, (!view.step1 && view.step>=1)};
        steps[1] = {L"Gemini AI analyzing...", view.step2, (!view.step2 && view.step>=2)};
        steps[2] = {L"Result ready \u2192 check overlay", (view.mode==1), false};
    }

    int y = 34;
    for(int i=0;i<3;i++){
        // Status icon
        COLORREF col;
        const wchar_t* icon;
        if(steps[i].done)   { icon=L" \u2714"; col=RGB(0,140,50);   }  // ✔ green
        else if(steps[i].active){ icon=sp;  col=RGB(120,80,0); }  // spinner yellow
        else                { icon=L" \u25CB"; col=RGB(90,90,115); }  // ○ dim

        SetTextColor(hdc,col);
        RECT iRc={6,y,28,y+17}; DrawTextW(hdc,icon,-1,&iRc,DT_CENTER|DT_SINGLELINE);

        // Description
        SetTextColor(hdc, steps[i].done?RGB(0,120,40):
                          steps[i].active?RGB(140,100,0):RGB(90,90,115));
        RECT dRc={30,y,rc.right-60,y+17};
        DrawTextW(hdc,steps[i].desc,-1,&dRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

        // Badge
        if(steps[i].done){
            SetTextColor(hdc,RGB(0,130,45));
            RECT bRc={rc.right-58,y,rc.right-6,y+17};
            DrawTextW(hdc,L"[ DONE ]",-1,&bRc,DT_RIGHT|DT_SINGLELINE);
        } else if(steps[i].active){
            SetTextColor(hdc,RGB(120,80,0));
            RECT bRc={rc.right-74,y,rc.right-6,y+17};
            DrawTextW(hdc,L"[ WORKING ]",-1,&bRc,DT_RIGHT|DT_SINGLELINE);
        } else {
            SetTextColor(hdc,RGB(55,55,70));
            RECT bRc={rc.right-66,y,rc.right-6,y+17};
            DrawTextW(hdc,L"[ WAIT ]",-1,&bRc,DT_RIGHT|DT_SINGLELINE);
        }
        y+=20;
    }

    // ── MCQ question snippet (shown during MCQ processing) ─────────────────
    if(isMCQ && !view.mcqQuestion.empty()){
        SetTextColor(hdc,RGB(80,80,60));
        RECT qRc={6,y+2,rc.right-6,y+16};
        std::wstring qSnip=L"\u00ab "+view.mcqQuestion;
        if(qSnip.size()>45) qSnip=qSnip.substr(0,44)+L"\u2026\u00bb";
        else qSnip+=L" \u00bb";
        DrawTextW(hdc,qSnip.c_str(),-1,&qRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        y+=18;
    }

    // ── Separator + status bar ─────────────────────────────────────────────
    hline(y+4, RGB(180,180,195));

    // Last log message
    SetTextColor(hdc,RGB(80,80,60));
    RECT sRc={6,y+7,rc.right-6,y+22};
    DrawTextW(hdc,(L"  "+AtoW(view.log.c_str())).c_str(),-1,&sRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

    // Pulse bar (animated)
    int barY=y+25, barH=8;
    RECT barbg={6,barY,rc.right-6,barY+barH};
    HBRUSH bb=CreateSolidBrush(RGB(220,220,220)); FillRect(hdc,&barbg,bb); DeleteObject(bb);
    int barW=(rc.right-12);
    int pulse=(int)(barW*0.4f);
    int offset=((view.spin*8)%(barW+pulse)) - pulse;
    int x0=6+std::max(0,offset), x1=6+std::min(barW,offset+pulse);
    if(x1>x0){RECT pRc={x0,barY,x1,barY+barH};
        HBRUSH pb=CreateSolidBrush(RGB(80,160,80));FillRect(hdc,&pRc,pb);DeleteObject(pb);}

    // F7=hide hint
    SetTextColor(hdc,RGB(100,90,40));
    RECT hRc={6,barY+10,rc.right-6,rc.bottom-2};
    DrawTextW(hdc,L"  F7 = Hide this box",-1,&hRc,DT_LEFT|DT_SINGLELINE);

    SelectObject(hdc,ofc); DeleteObject(fnt);
}

// ── Draw mode 0: Shortcut cheatsheet ──────────────────────────────────────
static void DrawShortcuts(HDC hdc, const RECT& rc,const OverlaySnapshot& view){
    HFONT fnt=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    HFONT of=(HFONT)SelectObject(hdc,fnt);

    // Title
    SetTextColor(hdc, RGB(0,140,50));
    RECT tRc={6,4,rc.right-6,20};
    DrawTextW(hdc,L"  \u2691 VEDA  |  Hotkey Reference  |  F7=Hide",-1,&tRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

    // Separator
    auto hline=[&](int y, COLORREF c){
        HPEN p=CreatePen(PS_SOLID,1,c);HPEN op2=(HPEN)SelectObject(hdc,p);
        MoveToEx(hdc,4,y,NULL);LineTo(hdc,rc.right-4,y);
        SelectObject(hdc,op2);DeleteObject(p);};
    hline(22,RGB(180,185,200));

    struct Row{const wchar_t* key; const wchar_t* desc; COLORREF c;} rows[]={
        {L"F8",       L"Extract text + AI answer",     RGB(0,130,45)},
        {L"Shift+F8", L"Cycle subject",                RGB(0,110,35)},
        {L"F6",       L"Toggle stealth typing ON/OFF", RGB(0,100,200)},
        {L"Ctrl+F6",  L"Load clipboard → buffer",      RGB(0,100,190)},
        {L"F9",       L"Extract MCQ options",          RGB(130,100,0)},
        {L"F12",      L"Reset buffer index to 0",      RGB(90,90,120)},
        {L"F7",       L"Hide / Show this box",         RGB(100,50,180)},
        {L"Alt+S",    L"Screenshot",                   RGB(80,80,110)},
        {L"Alt+Q",    L"Quit Veda",                    RGB(180,30,30)},
    };

    int y=26;
    for(auto& r:rows){
        SetTextColor(hdc, r.c);
        RECT kRc={6,y,95,y+15};
        DrawTextW(hdc,(std::wstring(L"  ")+r.key).c_str(),-1,&kRc,DT_LEFT|DT_SINGLELINE);
        SetTextColor(hdc, RGB(80,80,100));
        RECT dRc={95,y,rc.right-6,y+15};
        DrawTextW(hdc,(std::wstring(L"\u2192 ")+r.desc).c_str(),-1,&dRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        y+=16;
    }

    hline(y+2, RGB(180,185,200));

    // Status row
    SetTextColor(hdc, view.verified?RGB(0,140,50):RGB(120,80,0));
    wchar_t st[100]={}; swprintf_s(st,100,L"  %s | Subj: %s",AtoW(view.status.c_str()).c_str(),SUBJECTS[view.subject]);
    RECT sRc={6,y+5,rc.right-6,rc.bottom-2};
    DrawTextW(hdc,st,-1,&sRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

    SelectObject(hdc,of); DeleteObject(fnt);
}

// ── Draw mode 1 (MCQ variant): Dedicated MCQ Result display ─────────────────
static void DrawMCQResult(HDC hdc, const RECT& rc, const OverlaySnapshot& view){
    int N = view.mcqOptCount; if(N<4)N=4; if(N>5)N=5;
    // Total scrollable content height estimate
    int contentH = 26 + (view.mcqQuestion.empty()?0:21) + N*18 + 38 + 16;
    int visH = (int)rc.bottom - 24;           // visible area below frozen header
    int maxScroll = std::max(0, contentH - visH);
    int scrollY = std::min(view.mcqScrollY, maxScroll);
    int rightEdge = (maxScroll>0) ? rc.right-6 : rc.right-4;

    // helper: horizontal line
    auto hline=[&](int yy, COLORREF c){
        if(yy<22||yy>rc.bottom) return;
        HPEN p=CreatePen(PS_SOLID,1,c);HPEN op=(HPEN)SelectObject(hdc,p);
        MoveToEx(hdc,4,yy,NULL);LineTo(hdc,rightEdge,yy);
        SelectObject(hdc,op);DeleteObject(p);};

    // ── Scrollbar ─────────────────────────────────────────────────────────
    if(maxScroll>0){
        RECT sbBg={rc.right-5,24,rc.right-1,rc.bottom-1};
        HBRUSH sbBr=CreateSolidBrush(RGB(230,230,230)); FillRect(hdc,&sbBg,sbBr); DeleteObject(sbBr);
        int sbH=(int)rc.bottom-25;
        int thumbH=std::max(14,(int)(sbH*((int)rc.bottom-24)/std::max(1,contentH)));
        int thumbY=24+(int)((sbH-thumbH)*scrollY/std::max(1,maxScroll));
        RECT sbThumb={rc.right-5,thumbY,rc.right-1,thumbY+thumbH};
        HBRUSH th=CreateSolidBrush(RGB(150,150,150)); FillRect(hdc,&sbThumb,th); DeleteObject(th);
    }

    // ── Frozen header (not scrolled) ──────────────────────────────────────
    {
        HFONT fntB=CreateFontW(-12,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        HFONT of=(HFONT)SelectObject(hdc,fntB);
        // Re-fill header background to cover any scroll bleed
        HBRUSH hdrBg=CreateSolidBrush(RGB(255,255,255));
        RECT hdrR={0,0,rc.right,23}; FillRect(hdc,&hdrR,hdrBg); DeleteObject(hdrBg);
        SetTextColor(hdc,RGB(130,90,0));
        wchar_t ttl[64]={};
        swprintf_s(ttl,64,L"  \u2726 MCQ RESULT (%d opts)  F7=Hide",N);
        RECT tRc={4,4,rightEdge,20};
        DrawTextW(hdc,ttl,-1,&tRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,of); DeleteObject(fntB);
        HPEN sep=CreatePen(PS_SOLID,1,RGB(170,170,180));
        HPEN osep=(HPEN)SelectObject(hdc,sep);
        MoveToEx(hdc,4,22,NULL);LineTo(hdc,rightEdge,22);
        SelectObject(hdc,osep);DeleteObject(sep);
    }

    HFONT fnt=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    HFONT ofc=(HFONT)SelectObject(hdc,fnt);

    // Clip drawing below frozen header
    HRGN clip=CreateRectRgn(0,23,rc.right,(int)rc.bottom-14);
    SelectClipRgn(hdc,clip);

    int y = 26 - scrollY;

    // ── Question snippet ─────────────────────────────────────────────────
    if(!view.mcqQuestion.empty()){
        if(y>=23&&y<rc.bottom-14){
            SetTextColor(hdc,RGB(80,80,60));
            RECT qRc={6,y,rightEdge,y+14};
            std::wstring ql=L" Q: "+view.mcqQuestion;
            DrawTextW(hdc,ql.c_str(),-1,&qRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        y+=16; hline(y,RGB(180,180,195)); y+=5;
    }

    // ── Options A-E ──────────────────────────────────────────────────────
    char correct=view.mcqLetter;
    static const wchar_t* OPT_CORNERS[]=
        {L"TOP-LEFT",L"TOP-RIGHT",L"BOT-LEFT",L"BOT-RIGHT",L"CENTER"};

    for(int i=0;i<N;i++){
        char thisOpt=(char)('A'+i);
        bool isCorrect=(thisOpt==correct);

        if(y<rc.bottom-14 && y+18>23){
            // Green highlight row for correct
            if(isCorrect){
                RECT hiRc={4,std::max(23,y-1),rightEdge,std::min((int)rc.bottom-14,y+17)};
                HBRUSH hiBr=CreateSolidBrush(RGB(200,255,210));
                FillRect(hdc,&hiRc,hiBr); DeleteObject(hiBr);
            }

            int rowY=y;
            if(rowY>=23){
                // Bullet
                SetTextColor(hdc, isCorrect?RGB(0,150,55):RGB(100,100,130));
                RECT bRc={5,rowY,18,rowY+14};
                DrawTextW(hdc,isCorrect?L"\u25CF":L"\u25CB",-1,&bRc,DT_CENTER|DT_SINGLELINE);

                // Option letter  "A:"
                SetTextColor(hdc, isCorrect?RGB(0,160,55):RGB(90,90,115));
                RECT lRc={18,rowY,33,rowY+14};
                wchar_t lb[4]={}; swprintf_s(lb,4,L"%c:",thisOpt);
                DrawTextW(hdc,lb,-1,&lRc,DT_LEFT|DT_SINGLELINE);

                const std::wstring& optTxt=view.mcqOptions[i];

                if(isCorrect){
                    // Corner badge  "[TOP-LEFT]"  on right
                    SetTextColor(hdc,RGB(120,80,0));
                    wchar_t cb[22]={}; swprintf_s(cb,22,L"[%s]",OPT_CORNERS[i]);
                    RECT cRc={rightEdge-98,rowY,rightEdge,rowY+14};
                    DrawTextW(hdc,cb,-1,&cRc,DT_RIGHT|DT_SINGLELINE);
                    // Option text in bright green
                    SetTextColor(hdc,RGB(0,150,55));
                    RECT tRc2={33,rowY,rightEdge-102,rowY+14};
                    std::wstring lbl=optTxt.empty()?L"\u2190 CORRECT ANSWER":optTxt;
                    DrawTextW(hdc,lbl.c_str(),-1,&tRc2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
                } else {
                    SetTextColor(hdc,RGB(100,100,120));
                    RECT tRc2={33,rowY,rightEdge,rowY+14};
                    std::wstring lbl=optTxt.empty()?L"\u2014":optTxt;
                    DrawTextW(hdc,lbl.c_str(),-1,&tRc2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
                }
            }
        }
        y+=18;
    }

    hline(y+2,RGB(180,180,195)); y+=8;

    // ── Mouse corner row ─────────────────────────────────────────────────
    if(y>=23&&y<rc.bottom-14){
        SetTextColor(hdc,RGB(120,80,0));
        RECT mRc={6,y,rightEdge,y+14};
        std::wstring mm=L" \u2192 Mouse \u2192 "+view.mcqCorner+L" corner";
        DrawTextW(hdc,mm.c_str(),-1,&mRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    y+=16;

    // Scroll hint
    if(maxScroll>0&&y>=23&&y<rc.bottom-14){
        SetTextColor(hdc,RGB(110,110,90));
        RECT sh={6,y,rightEdge,y+13};
        DrawTextW(hdc,L" \u2191\u2193 scroll",-1,&sh,DT_LEFT|DT_SINGLELINE);
    }

    // Remove clip
    SelectClipRgn(hdc,NULL); DeleteObject(clip);

    // ── Pinned footer: log line ───────────────────────────────────────────
    {
        HBRUSH foot=CreateSolidBrush(RGB(255,255,255));
        RECT footBg={0,rc.bottom-14,rc.right,rc.bottom};
        FillRect(hdc,&footBg,foot); DeleteObject(foot);
        SetTextColor(hdc,RGB(120,120,100));
        RECT sRc={5,rc.bottom-13,rightEdge,rc.bottom-1};
        DrawTextW(hdc,(L" "+AtoW(view.log.c_str())).c_str(),-1,&sRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    SelectObject(hdc,ofc); DeleteObject(fnt);
}

// ── Draw mode 1: AI Answer ─────────────────────────────────────────────────
static void DrawAnswer(HDC hdc, const RECT& rc,const OverlaySnapshot& view){
    LONG idx=view.index; int total=(int)view.total;
    int pct=total>0?(int)(idx*100/total):0;
    bool stOn=view.typing;

    // Split the answer into lines
    std::vector<std::wstring> ansLines;
    const std::wstring& ans=view.answer;
    size_t pos=0;
    // skip leading blank lines
    while(pos<ans.size()&&(ans[pos]==L'\n'||ans[pos]==L'\r')) pos++;
    while(pos<=ans.size()){
        size_t nl=ans.find(L'\n',pos);
        std::wstring line=(nl==std::wstring::npos)?ans.substr(pos):ans.substr(pos,nl-pos);
        if(!line.empty()&&line.back()==L'\r') line.pop_back();
        ansLines.push_back(line);
        if(nl==std::wstring::npos) break;
        pos=nl+1;
    }

    // Layout constants
    const int HEADER_H = 23;   // frozen title area height
    const int FOOTER_H = 54;   // frozen footer: progress bar + log
    const int LINE_H   = 15;
    const int PAD      = 2;
    int visH = (int)rc.bottom - HEADER_H - FOOTER_H;   // scrollable viewport height
    int contentH = (int)ansLines.size() * LINE_H + PAD*2;
    int maxScroll = std::max(0, contentH - visH);
    int scrollY   = std::min(view.ansScrollY, maxScroll);
    int rightEdge = maxScroll>0 ? (int)rc.right-6 : (int)rc.right-4;

    // helper
    auto hline=[&](int y, COLORREF c){
        HPEN p=CreatePen(PS_SOLID,1,c);HPEN op=(HPEN)SelectObject(hdc,p);
        MoveToEx(hdc,4,y,NULL);LineTo(hdc,rightEdge,y);
        SelectObject(hdc,op);DeleteObject(p);};

    // ── Frozen header ────────────────────────────────────────────────────
    {
        // Clear header strip
        HBRUSH hb=CreateSolidBrush(RGB(255,255,255));
        RECT hR={0,0,(int)rc.right,HEADER_H}; FillRect(hdc,&hR,hb); DeleteObject(hb);
        HFONT fntB=CreateFontW(-12,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        HFONT of=(HFONT)SelectObject(hdc,fntB);
        SetTextColor(hdc, stOn?RGB(0,140,50):RGB(30,90,210));
        wchar_t title[100]={};
        swprintf_s(title,100,L"  AI Answer | %s | %d/%d (%d%%) | F7=Hide",
            SUBJECTS[view.subject],idx,total,pct);
        RECT tRc={4,4,rightEdge,20};
        DrawTextW(hdc,title,-1,&tRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,of); DeleteObject(fntB);
        hline(HEADER_H-1,RGB(180,185,200));
    }

    // ── Scrollbar ────────────────────────────────────────────────────────
    if(maxScroll>0){
        RECT sbBg={(int)rc.right-5,HEADER_H,(int)rc.right-1,(int)rc.bottom-FOOTER_H};
        HBRUSH sbBr=CreateSolidBrush(RGB(230,230,230)); FillRect(hdc,&sbBg,sbBr); DeleteObject(sbBr);
        int sbH=(int)rc.bottom-FOOTER_H-HEADER_H;
        int thumbH=std::max(12,(int)(sbH*visH/std::max(1,contentH)));
        int thumbY=HEADER_H+(int)((sbH-thumbH)*scrollY/std::max(1,maxScroll));
        RECT sbTh={(int)rc.right-5,thumbY,(int)rc.right-1,thumbY+thumbH};
        HBRUSH th=CreateSolidBrush(stOn?RGB(0,150,60):RGB(50,110,210)); FillRect(hdc,&sbTh,th); DeleteObject(th);
    }

    // ── Scrollable content area (clipped) ────────────────────────────────
    {
        HRGN clip=CreateRectRgn(0,HEADER_H,(int)rc.right,(int)rc.bottom-FOOTER_H);
        SelectClipRgn(hdc,clip);

        HFONT fntC=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        HFONT ofc=(HFONT)SelectObject(hdc,fntC);
        SetTextColor(hdc, RGB(30,30,30));

        int y = HEADER_H + PAD - scrollY;
        for(size_t li=0; li<ansLines.size(); li++){
            if(y+LINE_H>HEADER_H && y<(int)rc.bottom-FOOTER_H){
                RECT lRc={6,y,rightEdge,y+LINE_H};
                DrawTextW(hdc,(L" "+ansLines[li]).c_str(),-1,&lRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            }
            y+=LINE_H;
        }

        // Scroll hint at bottom of content (when scrollable)
        if(maxScroll>0){
            int hintY=(int)rc.bottom-FOOTER_H-14;
            HBRUSH hintBg=CreateSolidBrush(RGB(255,255,255));
            RECT hintR={0,hintY,(int)rc.right,(int)rc.bottom-FOOTER_H};
            FillRect(hdc,&hintR,hintBg); DeleteObject(hintBg);
            SetTextColor(hdc,RGB(100,100,130));
            RECT hRc={6,hintY,rightEdge,hintY+13};
            DrawTextW(hdc,L" \u2191\u2193 mouse wheel to scroll",-1,&hRc,DT_LEFT|DT_SINGLELINE);
        }

        SelectObject(hdc,ofc); DeleteObject(fntC);
        SelectClipRgn(hdc,NULL); DeleteObject(clip);
    }

    // ── Frozen footer: separator + progress bar + log ────────────────────
    {
        int footTop=(int)rc.bottom-FOOTER_H;
        HBRUSH fb=CreateSolidBrush(RGB(255,255,255));
        RECT fR={0,footTop,(int)rc.right,(int)rc.bottom}; FillRect(hdc,&fR,fb); DeleteObject(fb);
        hline(footTop+1,RGB(180,185,200));

        // Progress bar
        HFONT fntS=CreateFontW(-11,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,DEFAULT_QUALITY,FIXED_PITCH,L"Consolas");
        HFONT ofs=(HFONT)SelectObject(hdc,fntS);
        int barTop=footTop+4, barBot=barTop+14;
        RECT barbg={6,barTop,(int)rc.right-6,barBot};
        HBRUSH bb=CreateSolidBrush(RGB(230,230,230)); FillRect(hdc,&barbg,bb); DeleteObject(bb);
        if(total>0){
            int fw=(int)((rc.right-12)*(long long)idx/total);
            RECT bf={6,barTop,6+fw,barBot};
            HBRUSH bf2=CreateSolidBrush(stOn?RGB(0,140,60):RGB(50,110,200));
            FillRect(hdc,&bf,bf2); DeleteObject(bf2);
        }
        SetTextColor(hdc,RGB(30,30,30));
        wchar_t prog[70]={}; swprintf_s(prog,70,L" Buffer: %d/%d (%d%%) | %s",
            idx,total,pct,stOn?L"STEALTH ON":L"STEALTH OFF");
        DrawTextW(hdc,prog,-1,&barbg,DT_LEFT|DT_SINGLELINE|DT_VCENTER);

        // Log line
        SetTextColor(hdc,RGB(80,80,100));
        RECT sRc={6,barBot+3,(int)rc.right-6,(int)rc.bottom-2};
        DrawTextW(hdc,(L" "+AtoW(view.log.c_str())).c_str(),-1,&sRc,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);

        SelectObject(hdc,ofs); DeleteObject(fntS);
    }
}

// ── Main overlay paint ─────────────────────────────────────────────────────
static void DrawOverlay(HWND hwnd){
    const auto view=SnapshotOverlay();
    PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
    RECT rc; GetClientRect(hwnd,&rc);

    // White background (20% opacity)
    HBRUSH bg=CreateSolidBrush(RGB(255,255,255)); FillRect(hdc,&rc,bg); DeleteObject(bg);

    // Border: green=stealth, blue=answer, yellow=verifying, dim=idle
    COLORREF bc = view.typing     ? RGB(0,130,45) :
                  (view.mode==1) ? RGB(30,90,210) :
                  (!view.verified) ? RGB(255,195,0) : RGB(160,165,185);
    HPEN bp=CreatePen(PS_SOLID,2,bc); HPEN op=(HPEN)SelectObject(hdc,bp);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,GetStockObject(NULL_BRUSH));
    Rectangle(hdc,1,1,rc.right-1,rc.bottom-1);
    SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(bp);

    SetBkMode(hdc,TRANSPARENT);

    if(view.mode==0)      DrawShortcuts(hdc,rc,view);
    else if(view.mode==1){
        if(view.mcqLetter) DrawMCQResult(hdc,rc,view);
        else               DrawAnswer(hdc,rc,view);
    }
    else if(view.mode==3){
        HFONT font=CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        HFONT old=(HFONT)SelectObject(hdc,font);
        SetTextColor(hdc,RGB(200,30,30));
        RECT title={8,5,rc.right-8,23};
        DrawTextW(hdc,L"Request failed | F7 hide/show",-1,&title,DT_SINGLELINE);
        SetTextColor(hdc,RGB(20,20,20));
        RECT body={8,27,rc.right-8,rc.bottom-23};
        DrawTextW(hdc,view.answer.c_str(),-1,&body,DT_WORDBREAK|DT_NOPREFIX|DT_END_ELLIPSIS);
        RECT footer={8,rc.bottom-20,rc.right-8,rc.bottom-3};
        DrawTextW(hdc,L"Ctrl+Shift+C: copy full debug",-1,&footer,DT_SINGLELINE);
        SelectObject(hdc,old);DeleteObject(font);
    }
    else                      DrawProcessing(hdc,rc,view);

    EndPaint(hwnd,&ps);
}

static LRESULT CALLBACK OvlWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_PAINT: DrawOverlay(hwnd); return 0;
    case WM_SC_SHOW:
        if(!g_stopping.load()) { ShowWindow(hwnd,SW_SHOWNA); InvalidateRect(hwnd,nullptr,FALSE); }
        return 0;
    case WM_SC_TOGGLE:
        ShowWindow(hwnd,IsWindowVisible(hwnd)?SW_HIDE:SW_SHOWNA); return 0;
    case WM_TIMER:{
        if(g_stopping.load()){DestroyWindow(hwnd);return 0;}
        {StateLock lock(g_stateMutex);++g_spinTick;}
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }

    case WM_MOUSEWHEEL:{
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        int step=(delta/WHEEL_DELTA)*20;  // 20px per notch
        { StateLock lock(g_stateMutex);
          int mode=g_overlayMode.load();
          if(mode==1&&g_mcqLetter!=0)
              g_mcqScrollY=std::max(0,g_mcqScrollY-step);
          else if(mode==1)
              g_ansScrollY=std::max(0,g_ansScrollY-step);
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_LBUTTONDOWN: ReleaseCapture();SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd,msg,wp,lp);
    }
}

static DWORD WINAPI OverlayThread(LPVOID){
    WNDCLASSEXW wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=OvlWndProc;
    wc.hInstance=g_hModule; wc.lpszClassName=L"SCOverlay2";
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassExW(&wc);
    int scrW=GetSystemMetrics(SM_CXSCREEN),scrH=GetSystemMetrics(SM_CYSCREEN);
    g_overlayHwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_LAYERED,
        L"SCOverlay2",L"SC HUD",WS_POPUP,
        scrW-OVL_W-10,scrH-OVL_H-50,OVL_W,OVL_H,
        NULL,NULL,g_hModule,NULL);
    if(!g_overlayHwnd){UnregisterClassW(L"SCOverlay2",g_hModule);return 1;} if(g_stopping.load()){DestroyWindow(g_overlayHwnd);g_overlayHwnd=nullptr;UnregisterClassW(L"SCOverlay2",g_hModule);return 0;}
    SetLayeredWindowAttributes(g_overlayHwnd,0,51,LWA_ALPHA);
    ShowWindow(g_overlayHwnd,SW_SHOWNA); UpdateWindow(g_overlayHwnd);
    SetTimer(g_overlayHwnd,1,250,NULL);
    MSG msg; while(GetMessageW(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(IsWindow(g_overlayHwnd)) DestroyWindow(g_overlayHwnd);
    g_overlayHwnd=nullptr; UnregisterClassW(L"SCOverlay2",g_hModule); return 0;
}
static void StartOverlay(){StartWorker(OverlayThread);}
static void StopOverlay(){HWND hwnd=g_overlayHwnd.load();if(hwnd)PostMessageW(hwnd,WM_CLOSE,0,0);}

/* ============================================================
   PART I: KEYBOARD HOOK
   ============================================================ */
static LRESULT CALLBACK KbHookProc(int nCode,WPARAM wParam,LPARAM lParam){
    if(nCode!=HC_ACTION) return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
    if(g_stopping.load()) return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
    KBDLLHOOKSTRUCT* k=(KBDLLHOOKSTRUCT*)lParam;
    bool dn=(wParam==WM_KEYDOWN||wParam==WM_SYSKEYDOWN);
    bool alt=(k->flags&LLKHF_ALTDOWN)!=0;
    if(k->dwExtraInfo==STEALTH_MAGIC) return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
    if(k->flags&LLKHF_INJECTED) return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
    if(dn&&alt&&k->vkCode=='Q'){RequestStop();return 1;}
    StateLock stateLock(g_stateMutex);
    bool ctrl=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;

    // KEYUP: suppress orphan KEYUP of suppressed key
    if(!dn){
        if(g_suppVK!=0&&k->vkCode==(DWORD)g_suppVK){InterlockedExchange(&g_suppVK,0);return 1;}
        return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
    }

    // Screenshot / quit
    if(alt&&k->vkCode=='S'){StartWorker(ScreenshotThread);return 1;}
    if(alt&&k->vkCode=='Q'){RequestStop();return 1;}

    // F8: extract + AI  (Shift+F8 = cycle subject)
    if(!alt&&(k->vkCode==VK_F8)){
        bool shift=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
        if(shift){StartWorker([](LPVOID)->DWORD{CycleSubject();return 0;});return 1;}
        // Generation is assigned when the managed worker is created.
        SetLog("Extracting...");
        if(!StartWorker(TextExtractThread,true))SetLog("Worker limit reached; try again.");return 1;}
    // F1: previous answer in history
    if(!alt&&!ctrl&&(k->vkCode==VK_F1)){
        if(!g_answerHistory.empty()&&g_histIdx>0){
            g_histIdx--;
            g_answerDisplay=g_answerHistory[g_histIdx].text;
            g_displaySubject=g_answerHistory[g_histIdx].subject;
            g_overlayMode=1;
            LoadIntoBuffer(g_answerHistory[g_histIdx].typeable?g_answerDisplay:std::wstring{});
            char buf[40]; sprintf(buf,"History %d/%d",(int)g_histIdx+1,(int)g_answerHistory.size());
            SetLog(buf);
            NotifyOverlay();
        } else { SetLog("No previous answer."); }
        return 1;
    }
    // F2: next answer in history
    if(!alt&&!ctrl&&(k->vkCode==VK_F2)){
        if(!g_answerHistory.empty()&&g_histIdx<(int)g_answerHistory.size()-1){
            g_histIdx++;
            g_answerDisplay=g_answerHistory[g_histIdx].text;
            g_displaySubject=g_answerHistory[g_histIdx].subject;
            g_overlayMode=1;
            LoadIntoBuffer(g_answerHistory[g_histIdx].typeable?g_answerDisplay:std::wstring{});
            char buf[40]; sprintf(buf,"History %d/%d",(int)g_histIdx+1,(int)g_answerHistory.size());
            SetLog(buf);
            NotifyOverlay();
        } else { SetLog("No next answer."); }
        return 1;
    }
    // F9: review-only MCQ suggestion
    if(!alt&&!ctrl&&(k->vkCode==VK_F9)){
        // Generation is assigned when the managed worker is created.
        SetLog("Extracting MCQ...");
        if(!StartWorker(MCQExtractThread,true))SetLog("Worker limit reached; try again.");return 1;}

    // Ctrl+Shift+C explicitly copies the current answer.
    if(ctrl&&(GetAsyncKeyState(VK_SHIFT)&0x8000)&&k->vkCode=='C'){
        StartWorker([](LPVOID)->DWORD{
            std::wstring answer;{StateLock lock(g_stateMutex);answer=g_answerDisplay;}
            if(answer.empty()){SetLog("No answer to copy.");return 0;}
            SetLog(SetClipboard(ToUtf8(answer))?"Answer copied.":"Could not copy answer.");return 0;
        });return 1;
    }
    // F6 controls
    if(k->vkCode==VK_F6){
        if(ctrl){StartWorker([](LPVOID)->DWORD{LoadClipboard_();return 0;});}
        else HandleF6_();
        return 1;
    }
    // F7: toggle overlay visibility (show/hide corner box)
    if(!alt&&!ctrl&&(k->vkCode==VK_F7)){
        NotifyOverlay(true);
        return 1;
    }
    // F12: reset index
    if(!alt&&!ctrl&&(k->vkCode==VK_F12)){
        InterlockedExchange(&g_bufIdx,0);InterlockedExchange(&g_suppVK,0);
        SetLog("Buffer index reset.");return 1;}

    // Stealth intercept
    if(g_stealthOn){
        DWORD vk=k->vkCode;
        if(vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_SHIFT||vk==VK_LSHIFT||vk==VK_RSHIFT||
           vk==VK_MENU||vk==VK_LMENU||vk==VK_RMENU||vk==VK_ESCAPE||vk==VK_LWIN||vk==VK_RWIN)
            return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
        if(vk>=VK_F1&&vk<=VK_F24) return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
        if(vk==VK_BACK||vk==VK_DELETE||vk==VK_LEFT||vk==VK_RIGHT||vk==VK_UP||vk==VK_DOWN||
           vk==VK_HOME||vk==VK_END||vk==VK_PRIOR||vk==VK_NEXT||vk==VK_INSERT||vk==VK_CAPITAL)
            return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
        if(ctrl||alt) return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
        // Key repeat guard
        if(g_suppVK!=0&&vk==(DWORD)g_suppVK) return 1;
        // Buffer exhausted?
        LONG idx=g_bufIdx;
        if(idx>=(LONG)g_buf.size()){
            InterlockedExchange(&g_stealthOn,0);InterlockedExchange(&g_suppVK,0);
            SetLog("Buffer done! Stealth OFF.");
            return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
        }
        wchar_t ch=g_buf[idx]; LONG nextIdx=idx+1;

        InterlockedExchange(&g_suppVK,(LONG)vk);
        if(!InjectChar(ch)){
            g_stealthOn=0;
            SetLog("Input delivery failed; typing stopped.");
            return 1;
        }
        InterlockedExchange(&g_bufIdx,nextIdx);
        char tmp[48]; if(ch==L'\n') sprintf(tmp,"Pos %d: [ENTER]",(int)nextIdx);
        else if(ch==L' ') sprintf(tmp,"Pos %d: [SPACE]",(int)nextIdx);
        else{char cb[8]={};WideCharToMultiByte(CP_ACP,0,&ch,1,cb,7,NULL,NULL);sprintf(tmp,"Pos %d: '%s'",(int)nextIdx,cb);}
        SetLog(tmp);
        return 1;
    }
    return CallNextHookEx(g_hKbHook,nCode,wParam,lParam);
}

/* ============================================================
   PART J: HOOK LOOP + STARTUP
   ============================================================ */
static void RunHookLoop(){
    g_hookThreadId.store(GetCurrentThreadId());
    MSG msg; PeekMessageW(&msg,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
    try {
        MakeHwid(); LoadUserConfig();
        g_configReady.store(!g_regNum.empty()&&!g_userName.empty());
        GdiplusStartupInput gsi;
        if(GdiplusStartup(&g_gdiplusToken,&gsi,nullptr)!=Ok) throw 1;
        if(g_stopping.load()) throw 1;
        g_hKbHook=SetWindowsHookExW(WH_KEYBOARD_LL,KbHookProc,g_hModule,0);
        if(!g_hKbHook) throw 1;
        std::wstring base=GetExtractBase(); CreateDirectoryW(base.c_str(),nullptr);
        WriteFile_(base+L"active.txt","SC DLL active. Alt+Q requests coordinated shutdown.\r\n");
        StartOverlay();
        StartWorker([](LPVOID)->DWORD{RunVerification();return 0;});
        while(!g_stopping.load()&&GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    } catch(...) { OutputDebugStringW(L"ScreenCapture: startup or message loop failed.\n"); }
    g_stopping.store(true); ++g_extractGen;
    if(g_hKbHook){UnhookWindowsHookEx(g_hKbHook);g_hKbHook=nullptr;}
    {StateLock lock(g_stateMutex);g_stealthOn=0;g_suppVK=0;}
    StopOverlay();
    // Never unload code or shut GDI+ down while a worker may still use it.
    JoinWorkers();
    if(g_gdiplusToken){GdiplusShutdown(g_gdiplusToken);g_gdiplusToken=0;}
    DeleteFileW((GetExtractBase()+L"active.txt").c_str());
    g_hookThreadId.store(0); g_configReady.store(false); g_verified.store(false);
    InterlockedExchange(&g_active,0);
}

extern "C" __declspec(dllexport)
void CALLBACK StartHook(HWND,HINSTANCE,LPSTR,int){
    if(InterlockedCompareExchange(&g_active,1,0)!=0) return;
    g_stopping.store(false);
    RunHookLoop();
}

BOOL APIENTRY DllMain(HMODULE hMod,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        g_hModule=hMod; DisableThreadLibraryCalls(hMod);
        wchar_t proc[MAX_PATH]={}; GetModuleFileNameW(nullptr,proc,MAX_PATH);
        const wchar_t* base=wcsrchr(proc,L'\\'); base=base?base+1:proc;
        if(_wcsicmp(base,L"explorer.exe")==0 && InterlockedCompareExchange(&g_active,1,0)==0){
            HANDLE h=CreateThread(nullptr,0,[](LPVOID)->DWORD{
                RunHookLoop();
                // Release the load reference only after all callbacks and workers end.
                FreeLibraryAndExitThread(g_hModule,0);
                return 0;
            },nullptr,0,nullptr);
            if(h) CloseHandle(h); else InterlockedExchange(&g_active,0);
        }
    }
    // No window, GDI+, or thread teardown under the loader lock.
    // Explicit hosts must let StartHook return before releasing their DLL reference.
    return TRUE;
}
