#pragma once
enum class HttpError { none,cancelled,invalid_url,transport,status,too_large,deadline };
struct HttpResult {
    HttpError error=HttpError::none;DWORD status=0,windowsError=0;std::string body;
    bool ok()const{return error==HttpError::none&&status>=200&&status<300;}
};
struct InternetHandle {
    HINTERNET value=nullptr;explicit InternetHandle(HINTERNET h):value(h){}
    ~InternetHandle(){if(value)InternetCloseHandle(value);}
    InternetHandle(const InternetHandle&)=delete;InternetHandle& operator=(const InternetHandle&)=delete;
};
template<class Reader,class Cancel,class Clock>
static HttpResult ReadHttpBody(DWORD status,Reader read,Cancel cancelled,Clock expired){
    HttpResult r;r.status=status;
    constexpr size_t limit=4*1024*1024;char buffer[8192];
    for(;;){
        if(cancelled()){r.error=HttpError::cancelled;break;}
        if(expired()){r.error=HttpError::deadline;break;}
        DWORD bytes=0,error=0;
        if(!read(buffer,sizeof(buffer),bytes,error)){r.error=HttpError::transport;r.windowsError=error;break;}
        if(bytes>sizeof(buffer)||bytes>limit-r.body.size()){r.error=HttpError::too_large;break;}
        if(!bytes){if(cancelled())r.error=HttpError::cancelled;else if(expired())r.error=HttpError::deadline;break;}
        r.body.append(buffer,bytes);
    }
    if(r.error!=HttpError::none)r.body.clear();
    else if(status<200||status>=300)r.error=HttpError::status;
    return r;
}
static HttpResult HttpRequest(const std::wstring& url,const std::string* body,DWORD timeoutMs){
    HttpResult r;if(RequestCancelled()){r.error=HttpError::cancelled;return r;}
    URL_COMPONENTSW p={};p.dwStructSize=sizeof(p);p.dwHostNameLength=p.dwUrlPathLength=p.dwExtraInfoLength=static_cast<DWORD>(-1);
    if(!InternetCrackUrlW(url.c_str(),0,0,&p)||p.nScheme!=INTERNET_SCHEME_HTTPS){r.error=HttpError::invalid_url;return r;}
    std::wstring host(p.lpszHostName,p.dwHostNameLength),path=p.dwUrlPathLength?std::wstring(p.lpszUrlPath,p.dwUrlPathLength):L"/";
    if(p.dwExtraInfoLength)path.append(p.lpszExtraInfo,p.dwExtraInfoLength);
    ULONGLONG deadline=GetTickCount64()+timeoutMs;
    auto fail=[&](){r.error=RequestCancelled()?HttpError::cancelled:HttpError::transport;r.windowsError=GetLastError();return r;};
    InternetHandle session(InternetOpenW(L"ScreenCapture/2",INTERNET_OPEN_TYPE_PRECONFIG,nullptr,nullptr,0));if(!session.value)return fail();
    DWORD timeout=std::min<DWORD>(timeoutMs,5000);
    InternetSetOptionW(session.value,INTERNET_OPTION_CONNECT_TIMEOUT,&timeout,sizeof(timeout));
    InternetSetOptionW(session.value,INTERNET_OPTION_SEND_TIMEOUT,&timeout,sizeof(timeout));
    InternetSetOptionW(session.value,INTERNET_OPTION_RECEIVE_TIMEOUT,&timeoutMs,sizeof(timeoutMs));
    InternetHandle connection(InternetConnectW(session.value,host.c_str(),p.nPort,nullptr,nullptr,INTERNET_SERVICE_HTTP,0,0));if(!connection.value)return fail();
    DWORD flags=INTERNET_FLAG_SECURE|INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_NO_AUTO_REDIRECT|INTERNET_FLAG_NO_COOKIES|INTERNET_FLAG_NO_UI;
    InternetHandle request(HttpOpenRequestW(connection.value,body?L"POST":L"GET",path.c_str(),nullptr,nullptr,nullptr,flags,0));if(!request.value)return fail();
    if(RequestCancelled()){r.error=HttpError::cancelled;return r;}
    if(GetTickCount64()>=deadline){r.error=HttpError::deadline;return r;}
    // HttpSendRequestW requires an explicit character count (unlike its ANSI variant).
    constexpr wchar_t headers[]=L"Accept: application/json\r\nContent-Type: application/json\r\n";
    if(!HttpSendRequestW(request.value,headers,static_cast<DWORD>(sizeof(headers)/sizeof(headers[0])-1),body?const_cast<char*>(body->data()):nullptr,body?static_cast<DWORD>(body->size()):0))return fail();
    DWORD size=sizeof(r.status);if(!HttpQueryInfoW(request.value,HTTP_QUERY_STATUS_CODE|HTTP_QUERY_FLAG_NUMBER,&r.status,&size,nullptr))return fail();
    return ReadHttpBody(r.status,[&](char* buffer,DWORD count,DWORD& bytes,DWORD& error){bool ok=InternetReadFile(request.value,buffer,count,&bytes)!=0;if(!ok)error=GetLastError();return ok;},[]{return RequestCancelled();},[&]{return GetTickCount64()>=deadline;});
}
