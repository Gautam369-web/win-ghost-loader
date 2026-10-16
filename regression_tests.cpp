// Includes the implementation to exercise its real helpers without installing a
// hook, starting the UI, capturing the screen, injecting input, or making HTTP calls.
#include "ScreenCaptureDLL.cpp"
#include <iostream>
#include <stdexcept>

static int checks=0;
static void Check(bool ok,const char* name){
    ++checks;
    if(!ok) throw std::runtime_error(name);
}
static std::string ReadTestFile(const std::wstring& path){
    FILE* f=_wfopen(path.c_str(),L"rb");
    if(!f) return {};
    std::string result; char block[1024]; size_t n;
    while((n=fread(block,1,sizeof(block),f)))result.append(block,n);
    fclose(f);return result;
}
static HANDLE gate=nullptr, started=nullptr;
static std::atomic<int> finished{0}, superseded{0};
static DWORD WINAPI WaitingWorker(LPVOID){
    SetEvent(started);
    WaitForSingleObject(gate,5000);
    if(RequestCancelled()) ++superseded;
    ++finished; return 0;
}
static DWORD WINAPI BufferWriter(LPVOID){
    for(int i=0;i<500;++i) LoadIntoBuffer(L"if ready:\r\n    run()\r\n");
    return 0;
}
struct FakeClipboard {
    bool openOk=true,emptyOk=true,setOk=true,closed=false;int emptied=0,sets=0;HGLOBAL held=nullptr;
    ~FakeClipboard(){if(held)GlobalFree(held);}
    bool open(HWND){return openOk;}
    bool empty(){++emptied;return emptyOk;}
    HANDLE set(HGLOBAL value){++sets;if(!setOk)return nullptr;held=value;return value;}
    void close(){closed=true;}
};

int main(){
    try{
        const std::string python="if ready:\n    for x in xs:\n        print(x)\n";
        Check(TrimStr("  #include <stdio.h>\n")=="#include <stdio.h>","C preprocessor retained");
        Check(JStr("{\"nested\":{\"status\":\"approved\"}}","status").empty(),"nested status cannot approve");
        Check(sc::ParseJson("{\"status\":\"blocked\",\"status\":\"approved\"}").is_discarded(),"duplicate status rejected");
        Check(sc::ParseJson("{\"status\":\"approved\"}junk").is_discarded(),"trailing junk rejected");
        Check(sc::ParseJson(std::string(70,'[')+"0"+std::string(70,']')).is_discarded(),"nesting limit");
        Check(ParseMCQLetter("Cannot determine")==0&&ParseMCQLetter("correct answer is C")==0,"ambiguous MCQ rejected");
        Check(ParseMCQLetter(" c\n")=='C',"single MCQ letter");
        Check(GeminiExtractText(R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"partial"}]}}]})").empty(),"partial answer rejected");
        Check(GeminiExtractText(R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"secret","thought":true},{"text":"hello"},{"text":" world"}]}}]})")=="hello world","answer parts joined without thoughts");
        auto no=[](){return false;};
        auto unused=[](char*,DWORD,DWORD&,DWORD&){throw std::runtime_error("reader should not run");return false;};
        auto denied=ReadHttpBody(403,[](char*,DWORD,DWORD& n,DWORD&){n=0;return true;},no,no);
        Check(denied.error==HttpError::status,"HTTP denial remains failure after reading");
        bool sentError=false;
        auto unavailable=ReadHttpBody(503,[&](char* b,DWORD,DWORD& n,DWORD&){
            std::string body=R"({"error":"AI is not ready. Ask the admin to test models and confirm project quotas."})";
            n=sentError?0:static_cast<DWORD>(body.size());if(n)memcpy(b,body.data(),n);sentError=true;return true;
        },no,no);
        Check(!unavailable.ok()&&!SafeServiceError(unavailable.body).empty(),"HTTP 503 preserves safe setup explanation");
        Check(SafeServiceError(R"({"error":"Access denied"})")=="Access denied","known error shown");
        Check(SafeServiceError(R"({"error":"secret provider data"})").empty(),"raw server error hidden");
        t_debug="F9 | HTTP 503";LoadIntoBuffer(L"old answer");
        PublishFailure("AI is not ready");
        auto failed=SnapshotOverlay();
        Check(failed.mode==3&&failed.total==0&&!failed.typing,"failure panel clears stale typing");
        Check(failed.answer.find(L"HTTP 503")!=std::wstring::npos,"failure panel includes debug");
        HttpResult verificationError;verificationError.status=503;verificationError.error=HttpError::status;
        VerificationFailure("Registration/verify",verificationError,12);
        auto verificationView=SnapshotOverlay();
        Check(verificationView.mode==3&&verificationView.answer.find(L"Registration/verify")!=std::wstring::npos,"startup verification opens diagnostic panel");
        Check(verificationView.status.find("HTTP 503")!=std::string::npos,"verification status keeps HTTP error for later F8 request");
        Check(ReadHttpBody(200,unused,[]{return true;},no).error==HttpError::cancelled,"HTTP cancellation");
        Check(ReadHttpBody(200,unused,no,[]{return true;}).error==HttpError::deadline,"HTTP deadline");
        int chunks=0;
        auto partial=ReadHttpBody(200,[&](char* b,DWORD,DWORD& n,DWORD& e){if(chunks++==0){b[0]='x';n=1;return true;}e=123;return false;},no,no);
        Check(partial.error==HttpError::transport&&partial.body.empty(),"incomplete HTTP body discarded");
        auto large=ReadHttpBody(200,[](char* b,DWORD count,DWORD& n,DWORD&){memset(b,'x',count);n=count;return true;},no,no);
        Check(large.error==HttpError::too_large&&large.body.empty(),"oversize HTTP discarded");
        {FakeClipboard api;Check(!PublishClipboardText(nullptr,L"test",api)&&api.emptied==0,"clipboard requires owner");}
        {FakeClipboard api;api.openOk=false;Check(!PublishClipboardText((HWND)1,L"test",api)&&api.emptied==0,"busy clipboard preserved");}
        {FakeClipboard api;api.emptyOk=false;Check(!PublishClipboardText((HWND)1,L"test",api)&&api.sets==0&&api.closed,"clipboard empty failure closes");}
        {FakeClipboard api;api.setOk=false;Check(!PublishClipboardText((HWND)1,L"test",api)&&api.closed,"clipboard set failure");}
        {FakeClipboard api;Check(PublishClipboardText((HWND)1,L"test",api)&&api.held&&api.closed,"clipboard ownership transferred");}
        Check(CleanAiResponse(python)==python,"plain Python indentation");
        Check(CleanAiResponse("```python\n"+python+"```\n")==python.substr(0,python.size()-1),"fenced nested Python");
        Check(CleanAiResponse("```python\r\nif x:\r\n\tgo()\r\n```\r\n")=="if x:\r\n\tgo()","CRLF fence and tabs");
        Check(CleanAiResponse("```\n```").empty(),"empty fence");
        Check(CleanAiResponse("x = '```'\n")=="x = '```'\n","literal internal fence");
        Check(CleanAiResponse("```python\n    x")=="```python\n    x","incomplete fence unchanged");
        Check(CleanAiResponse("\n    text\n\n")=="\n    text\n\n","plain whitespace preserved");
        Check(sc::NormalizeNewlines(L"a\r\n    b\rc\n")==L"a\n    b\nc\n","line ending normalization");

        const std::string name="A \"quoted\" name\\path\n\t";
        const std::string key="test-only-key\\\"\t";
        std::string config=sc::UserConfig(name,"REG","Python",key);
        Check(JStr(config,"name")==name,"escaped name round trip");
        Check(JStr(config,"personal_api_key")==key,"personal key round trip");
        Check(JStr(sc::UserConfig(name,"REG","Java",JStr(config,"personal_api_key")),"personal_api_key")==key,"subject change preserves key");
        Check(JStr("{\"name\":\n\t\"value\"}","name")=="value","JSON whitespace");
        Check(JStr("{\"name\":\"\\uD83D\\uDE00\"}","name")=="\xf0\x9f\x98\x80","JSON surrogate pair");
        Check(JStr("{\"name\":\"\\uD800\"}","name").empty(),"invalid surrogate rejected");
        Check(JStr("{\"name\":\"unterminated}","name").empty(),"truncated JSON string rejected");
        Check(JStr("{\"note\":\"name\",\"name\":\"actual\"}","name")=="actual","key-like value ignored");
        std::string controls;
        for(int c=1;c<32;++c)controls+=static_cast<char>(c);
        Check(JStr("{\"name\":"+sc::JsonQuote(controls)+"}","name")==controls,"all JSON control characters");

        // Only a process-specific scratch directory is touched.
        std::wstring dir=L"regression_scratch_"+std::to_wstring(GetCurrentProcessId());
        Check(CreateDirectoryW(dir.c_str(),nullptr)!=0,"create scratch directory");
        std::wstring file=dir+L"\\config_\u6d4b\u8bd5.json";
        Check(WriteConfigAtomic(file,config),"atomic create with Unicode path");
        std::string changed=sc::UserConfig(name,"REG","Java",key);
        Check(WriteConfigAtomic(file,changed),"atomic replacement");
        Check(ReadTestFile(file)==changed,"replacement contents");
        HANDLE held=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        Check(held!=INVALID_HANDLE_VALUE,"lock destination for failure test");
        Check(!WriteConfigAtomic(file,"bad replacement"),"locked replacement reports failure");
        Check(ReadTestFile(file)==changed,"failed replacement preserves original");
        CloseHandle(held);
        DeleteFileW(file.c_str());
        Check(RemoveDirectoryW(dir.c_str())!=0,"temporary files cleaned on failure");

        g_stopping=false;
        LoadIntoBuffer(L"if ready:\r\n    run()\r\n");
        g_stealthOn=1; g_bufIdx=5;
        LoadIntoBuffer(L"  preserved\r\n\tindent");
        Check(g_buf==L"  preserved\n\tindent"&&g_bufIdx==0&&!g_stealthOn,"buffer replacement is normalized and disarmed");
        Check(StartWorker(BufferWriter),"start buffer writer");
        Check(StartWorker(BufferWriter),"start concurrent buffer writer");
        for(int i=0;i<500;++i){
            StateLock lock(g_stateMutex);
            Check(g_bufIdx==0&&g_buf.find(L'\r')==std::wstring::npos,"buffer snapshot remains coherent");
        }
        JoinWorkers();

        gate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        started=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        Check(gate&&started,"worker test events");
        Check(StartWorker(WaitingWorker,true),"start first request");
        Check(WaitForSingleObject(started,5000)==WAIT_OBJECT_0,"first request entered");
        ResetEvent(started);
        Check(StartWorker(WaitingWorker,true),"start superseding request");
        Check(WaitForSingleObject(started,5000)==WAIT_OBJECT_0,"second request entered");
        SetEvent(gate); JoinWorkers();
        Check(finished==2&&superseded==1,"generation belongs to original request");

        finished=0;superseded=0;ResetEvent(gate);ResetEvent(started);
        Check(StartWorker(WaitingWorker),"start shutdown worker");
        Check(WaitForSingleObject(started,5000)==WAIT_OBJECT_0,"shutdown worker entered");
        RequestStop();
        Check(!StartWorker(WaitingWorker),"shutdown rejects new work");
        SetEvent(gate);JoinWorkers();
        Check(finished==1&&superseded==1&&g_workers.empty(),"shutdown joins and closes tracked workers");
        CloseHandle(gate);CloseHandle(started);
        std::cout<<"PASS: "<<checks<<" checks; no hooks, capture, input, or network used.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
