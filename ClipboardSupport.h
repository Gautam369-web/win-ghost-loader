#pragma once
struct ClipboardApi {
    bool open(HWND owner){return OpenClipboard(owner)!=0;}
    bool empty(){return EmptyClipboard()!=0;}
    HANDLE set(HGLOBAL memory){return SetClipboardData(CF_UNICODETEXT,memory);}
    void close(){CloseClipboard();}
};
template<class Api>
static bool PublishClipboardText(HWND owner,const std::wstring& text,Api& api){
    if(!owner)return false;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
    if(!memory)return false;
    auto target=static_cast<wchar_t*>(GlobalLock(memory));
    if(!target){GlobalFree(memory);return false;}
    memcpy(target,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);
    if(!api.open(owner)){GlobalFree(memory);return false;}
    bool ok=api.empty()&&api.set(memory)!=nullptr;
    api.close();if(!ok)GlobalFree(memory);return ok;
}
