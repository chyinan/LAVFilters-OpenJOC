// SDK-free fixture patterned on the repository's GainPersistenceTests.
// Seven unchanged production methods are tested. Lock and registry adapters
// are deterministic; decoder configuration/delivery are non-failing adapters.
// This is NOT COM marshalling, Windows registry, DirectShow, or playback coverage.
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include "PolicyDefinitions.inc"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <thread>
using LPCWSTR=const wchar_t*;
using LAVOpenJocHrtfSource=int; using LAVOpenJocBinauralVirtualLayout=int;
// Only built-in HRTF/layout selections are exercised; no SOFA or decoder I/O.
bool IsLAVOpenJocHrtfSource(int v){return v==0;}
bool IsLAVOpenJocBinauralVirtualLayout(int v){return v==0;}
struct LAVOpenJocOutputContract {LAVOpenJocOutputPolicy policy;};
const LAVOpenJocOutputContract* FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy p){
    static const std::array<LAVOpenJocOutputContract,8> contracts={{{LAVOpenJocOutputPolicy::Stereo},{LAVOpenJocOutputPolicy::Layout51},{LAVOpenJocOutputPolicy::Layout71},{LAVOpenJocOutputPolicy::Layout512},{LAVOpenJocOutputPolicy::Layout514},{LAVOpenJocOutputPolicy::Layout712},{LAVOpenJocOutputPolicy::Layout714},{LAVOpenJocOutputPolicy::Binaural}}};
    const auto index=static_cast<unsigned>(p); return index<contracts.size()?&contracts[index]:nullptr;
}
using HRESULT=int; using BOOL=bool; using DWORD=std::uint32_t; using HKEY=void*;
constexpr HRESULT S_OK=0,S_FALSE=1,E_INVALIDARG=-1,E_POINTER=-2,E_FAIL=-3;
constexpr BOOL TRUE=true,FALSE=false;
constexpr HKEY HKEY_CURRENT_USER=nullptr;
constexpr wchar_t kOpenJocOutputPolicyVersionValue[]=L"OpenJocOutputPolicyVersion";
constexpr wchar_t kOpenJocOutputPolicyValue[]=L"OpenJocOutputPolicy";
#define LAVC_AUDIO_REGISTRY_KEY L"in-memory test only"
#define LAV_OPENJOC_SIDE_BY_SIDE
#define SAFE_DELETE(p) do {delete (p); (p)=nullptr;} while(false)
#define SUCCEEDED(hr) ((hr)>=0)
#define FAILED(hr) ((hr)<0)
#define HRESULT_FROM_WIN32(hr) (-(hr))
#define CheckPointer(p,hr) do {if(!(p))return(hr);} while(false)
void require(bool ok,const char*detail) {if(!ok){std::cerr<<detail<<'\n';std::abort();}}
struct Schedule {
    std::mutex mutex; std::condition_variable cv;
    bool enabled=false,paused=false,attempted=false,acquired=false,finished=false;
    template<class P> void wait(std::unique_lock<std::mutex>&l,P p) {
        require(cv.wait_for(l,std::chrono::seconds(5),p),"schedule timeout");
    }
} schedule;
thread_local bool competitor=false;
struct CCritSec {
    std::recursive_mutex mutex;
    void Lock(){
        if(!competitor||!schedule.enabled){mutex.lock();return;}
        const bool acquired=mutex.try_lock();
        {std::lock_guard<std::mutex> l(schedule.mutex);schedule.attempted=true;schedule.acquired=acquired;schedule.cv.notify_all();}
        if(!acquired)mutex.lock();
    }
    void Unlock(){mutex.unlock();}
};
struct CAutoLock {CCritSec*p; explicit CAutoLock(CCritSec*v):p(v){p->Lock();}~CAutoLock(){p->Unlock();}};
std::atomic<DWORD> saved_policy{0},saved_version{1};
std::atomic<int> writes{0};
bool CreateRegistryKey(HKEY,const wchar_t*){return true;}
int GetLastError(){return 0;}
bool ReadExactRegistryDword(HKEY,const wchar_t*,const wchar_t*n,DWORD*v){
    *v=std::wcscmp(n,kOpenJocOutputPolicyValue)==0?saved_policy.load():saved_version.load();return true;
}
struct CRegistry {
    CRegistry(HKEY,const wchar_t*,HRESULT&hr){hr=S_OK;}
    HRESULT WriteDWORD(const wchar_t*n,DWORD value){
        if(std::wcscmp(n,kOpenJocOutputPolicyVersionValue)==0){saved_version=value;return S_OK;}
        if(schedule.enabled&&!competitor){
            std::unique_lock<std::mutex>l(schedule.mutex);schedule.paused=true;schedule.cv.notify_all();
            schedule.wait(l,[]{return schedule.attempted;});
            if(schedule.acquired)schedule.wait(l,[]{return schedule.finished;});
        }
        saved_policy=value;++writes;return S_OK;
    }
};
struct CLAVAudio {
    CCritSec m_csReceive;
    struct Settings {LAVOpenJocOutputPolicy OpenJocOutputPolicy=LAVOpenJocOutputPolicy::Stereo;int OpenJocHrtfSource=0,OpenJocBinauralVirtualLayout=0;std::wstring OpenJocCustomSofaPath;}m_settings;
    struct Decoder {const LAVOpenJocOutputContract*c=FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Stereo);
        const LAVOpenJocOutputContract*OutputContract(){return c;}
        bool SetOutputPolicy(LAVOpenJocOutputPolicy p){c=FindLAVOpenJocOutputContract(p);return c!=nullptr;}
    }m_openJoc;
    struct Buffer {void Clear(){}}m_buff;
    std::atomic<LAVOpenJocOutputPolicy>m_openJocOutputPolicySnapshot{LAVOpenJocOutputPolicy::Stereo};
    BOOL m_bRuntimeConfig=FALSE,m_bQueueResync=FALSE,m_bResyncTimestamp=FALSE;
    int *m_pTrayIcon=nullptr;
    void RefreshOpenJocAdmissionSnapshot(){} void FlushOutput(BOOL){
        LAVOpenJocOutputPolicy p; require(GetOutputPolicy(&p)==S_OK,"flush readback");
    }
    HRESULT LoadSettings(){m_settings.OpenJocOutputPolicy=LAVOpenJocOutputPolicy::Stereo; m_openJocOutputPolicySnapshot=LAVOpenJocOutputPolicy::Stereo; return m_bRuntimeConfig?S_FALSE:LoadOpenJocOutputPolicySettings();}
    HRESULT SetRuntimeConfig(BOOL);
    HRESULT SetBinauralConfiguration(LAVOpenJocOutputPolicy,int,int,LPCWSTR);
    HRESULT SaveOpenJocBinauralSettings(){return m_bRuntimeConfig?S_FALSE:S_OK;}
    HRESULT ConfigureOpenJocBinauralConfiguration(LAVOpenJocOutputPolicy p,int,int,const wchar_t*,bool clear){
        // Valid speaker output selected through the property-page API.
        require(p!=LAVOpenJocOutputPolicy::Binaural,"fixture uses speaker output only");
        return ConfigureOpenJocOutputPolicy(p,clear);
    }
    HRESULT GetOutputPolicy(LAVOpenJocOutputPolicy*);
    HRESULT SetOutputPolicy(LAVOpenJocOutputPolicy);
    HRESULT ConfigureOpenJocOutputPolicy(LAVOpenJocOutputPolicy,bool);
    HRESULT SaveOpenJocOutputPolicySettings(LAVOpenJocOutputPolicy);
    HRESULT LoadOpenJocOutputPolicySettings();
};
#include "PolicyProductionMethods.inc"
bool concurrent_case(int mode){
    CLAVAudio audio; saved_policy=0; saved_version=1; writes=0;
    schedule.paused=schedule.attempted=schedule.acquired=schedule.finished=false;
    schedule.enabled=true;
    std::thread a([&]{require(audio.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51)==S_OK,"A setter");});
    {std::unique_lock<std::mutex>l(schedule.mutex);schedule.wait(l,[]{return schedule.paused;});}
    std::thread b([&]{
        competitor=true;
        HRESULT hr= mode==0 ? audio.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714) :
                    mode==1 ? audio.SetBinauralConfiguration(LAVOpenJocOutputPolicy::Layout714,0,0,L"") :
                    audio.SetRuntimeConfig(mode==2);
        require(hr==S_OK,"B operation");
        std::lock_guard<std::mutex>l(schedule.mutex);schedule.finished=true;schedule.cv.notify_all();
    });
    a.join();b.join();schedule.enabled=false;
    LAVOpenJocOutputPolicy live;require(audio.GetOutputPolicy(&live)==S_OK,"live getter");
    CLAVAudio reload;require(reload.LoadOpenJocOutputPolicySettings()==S_OK,"reload");
    const auto expected_saved=mode<2?LAVOpenJocOutputPolicy::Layout714:LAVOpenJocOutputPolicy::Layout51;
    const auto expected_live=mode==2?LAVOpenJocOutputPolicy::Stereo:expected_saved;
    const bool ok=!schedule.acquired && live==expected_live && saved_policy==static_cast<DWORD>(expected_saved) && reload.m_openJocOutputPolicySnapshot==expected_saved && writes==(mode<2?2:1);
    std::cout<<(ok?"PASS ":"FAIL ")<<"concurrency mode="<<mode<<" live="<<static_cast<DWORD>(live)<<" saved="<<saved_policy<<" reloaded="<<static_cast<DWORD>(reload.m_openJocOutputPolicySnapshot.load())<<"\n";
    return ok;
}
int main(){
    CLAVAudio serial;
    require(serial.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51)==S_OK,"serial 5.1");
    require(serial.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714)==S_OK,"serial 7.1.4");
    require(saved_policy==6 && serial.m_openJocOutputPolicySnapshot==LAVOpenJocOutputPolicy::Layout714,"serial must agree");
    require(serial.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714)==S_OK && saved_policy==6,"unchanged policy");
    std::cout<<"PASS serial/unchanged setters: live=6 saved=6\n";
    CLAVAudio runtime; require(runtime.SetRuntimeConfig(TRUE)==S_OK,"runtime enable"); int oldwrites=writes;
    require(runtime.SetOutputPolicy(LAVOpenJocOutputPolicy::Layout51)==S_OK && writes==oldwrites,"runtime-only must not persist");
    require(runtime.SetRuntimeConfig(FALSE)==S_OK && runtime.m_openJocOutputPolicySnapshot==LAVOpenJocOutputPolicy::Layout714,"persistent reload");
    std::cout<<"PASS runtime-only guard and persistent reload\n";
    bool ok=true;
    for(int mode=0;mode<4;++mode) ok=concurrent_case(mode)&&ok;
    std::cout<<"Portable production-method regression; no real COM/Windows execution\n";
    return ok?0:1;
}
