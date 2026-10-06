// The runner extracts the actual production methods without rewriting their bodies.
// Adapters replace only DirectShow locking/delivery and Win32 registry dependencies.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <thread>

using HRESULT = int;
using BOOL = bool;
using DWORD = std::uint32_t;
using LONG = int;
using HKEY = void *;
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_INVALIDARG = -1, E_POINTER = -2, E_FAIL = -3;
constexpr BOOL TRUE = true, FALSE = false;
constexpr HKEY HKEY_CURRENT_USER = nullptr;
constexpr int ERROR_SUCCESS = 0, REG_OPTION_NON_VOLATILE = 0, KEY_WRITE = 1, KEY_WOW64_64KEY = 2;
constexpr int LAV_OPENJOC_OUTPUT_GAIN_DEFAULT_TENTHS_DB = 0;
constexpr int LAV_OPENJOC_OUTPUT_GAIN_MIN_TENTHS_DB = -200, LAV_OPENJOC_OUTPUT_GAIN_MAX_TENTHS_DB = 200;
constexpr DWORD LAV_OPENJOC_OUTPUT_GAIN_SCHEMA_VERSION = 1;
constexpr wchar_t kOpenJocOutputGainVersionValue[] = L"OpenJocOutputGainVersion";
constexpr wchar_t kOpenJocOutputGainValue[] = L"OpenJocOutputGainTenthsDb";
#define LAVC_AUDIO_REGISTRY_KEY L"test"
#define LAV_OPENJOC_SIDE_BY_SIDE
#define FAILED(hr) ((hr) < 0)
#define HRESULT_FROM_WIN32(hr) (-(hr))
#define CheckPointer(p, hr) do { if (!(p)) return (hr); } while (false)
#define SAFE_DELETE(p) do { delete (p); (p) = nullptr; } while (false)

void require(bool condition, const char *message)
{
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
}

// A paused first registry write learns whether the competing operation could
// acquire the receive lock. If it could, finish that operation before releasing
// the older write. If it could not, let the first transaction finish. No sleeps
// or probabilistic races: timeouts only turn a broken harness/deadlock into failure.
struct Schedule
{
    std::mutex mutex;
    std::condition_variable cv;
    bool paused = false, attempted = false, acquired = false, finished = false;
    bool enabled = false;
    template<class Predicate> void wait(std::unique_lock<std::mutex>& lock, Predicate predicate)
    {
        require(cv.wait_for(lock, std::chrono::seconds(5), predicate), "schedule timed out");
    }
} schedule;
thread_local bool competitor = false;
struct CCritSec
{
    std::recursive_mutex mutex;
    void Lock()
    {
        if (!competitor || !schedule.enabled) { mutex.lock(); return; }
        const bool acquired = mutex.try_lock();
        {
            std::lock_guard<std::mutex> lock(schedule.mutex);
            schedule.attempted = true;
            schedule.acquired = acquired;
            schedule.cv.notify_all();
        }
        if (!acquired) mutex.lock();
    }
    void Unlock() { mutex.unlock(); }
};
struct CAutoLock
{
    CCritSec *lock;
    explicit CAutoLock(CCritSec *value) : lock(value) { lock->Lock(); }
    ~CAutoLock() { lock->Unlock(); }
};
std::atomic<DWORD> saved_gain{0}, saved_version{1};
std::atomic<int> writes{0};
bool fail_save = false;
LONG RegCreateKeyExW(HKEY, const wchar_t *, int, void *, int, int, void *, HKEY *key, void *)
{
    *key = nullptr;
    return fail_save ? 5 : ERROR_SUCCESS;
}
void RegCloseKey(HKEY) {}
bool ReadExactRegistryDword(HKEY, const wchar_t *, const wchar_t *name, DWORD *value)
{
    *value = std::wcscmp(name, kOpenJocOutputGainValue) == 0 ? saved_gain.load() : saved_version.load();
    return true;
}
struct CRegistry
{
    CRegistry(HKEY, const wchar_t *, HRESULT &hr, BOOL, BOOL) { hr = S_OK; }
    HRESULT WriteDWORD(const wchar_t *name, DWORD value)
    {
        if (std::wcscmp(name, kOpenJocOutputGainVersionValue) == 0)
        { saved_version = value; return S_OK; }
        if (schedule.enabled && !competitor)
        {
            std::unique_lock<std::mutex> lock(schedule.mutex);
            schedule.paused = true;
            schedule.cv.notify_all();
            schedule.wait(lock, [] { return schedule.attempted; });
            if (schedule.acquired) schedule.wait(lock, [] { return schedule.finished; });
        }
        saved_gain = value;
        ++writes;
        return S_OK;
    }
};
bool IsLAVOpenJocOutputGainTenthsDb(int gain)
{ return gain >= LAV_OPENJOC_OUTPUT_GAIN_MIN_TENTHS_DB && gain <= LAV_OPENJOC_OUTPUT_GAIN_MAX_TENTHS_DB; }
struct CLAVAudio
{
    CCritSec m_csReceive;
    struct { int OpenJocOutputGainTenthsDb = 0; } m_settings;
    struct { bool openjoc_contract = false; } m_OutputQueue;
    std::atomic<std::int32_t> m_openJocOutputGainSnapshot{0};
    BOOL m_bRuntimeConfig = FALSE;
    int *m_pTrayIcon = nullptr;
    int flushes = 0;
    HRESULT flush_result = S_OK;
    bool reenter = false;
    HRESULT FlushOutputLocked(BOOL)
    {
        ++flushes;
        if (reenter)
        {
            std::int32_t value;
            require(GetOutputGain(&value) == S_OK, "recursive gain readback failed");
        }
        return flush_result;
    }
    // Only unrelated settings/decoder reconfiguration are omitted. Gain reload
    // itself below is the actual production method, as are the runtime mode
    // transition, getter, setter, and registry saver.
    HRESULT LoadSettings()
    {
        m_settings.OpenJocOutputGainTenthsDb = 0;
        m_openJocOutputGainSnapshot.store(0, std::memory_order_release);
        return m_bRuntimeConfig ? S_FALSE : LoadOpenJocOutputGainSettings();
    }
    HRESULT LoadOpenJocOutputGainSettings();
    HRESULT SaveOpenJocOutputGainSettings(std::int32_t);
    HRESULT SetRuntimeConfig(BOOL);
    HRESULT GetOutputGain(std::int32_t *);
    HRESULT SetOutputGain(std::int32_t);
};
#include "ProductionMethods.inc"

bool concurrent_case(int mode)
{
    CLAVAudio audio;
    saved_gain = 0; saved_version = 1; writes = 0;
    schedule.paused = schedule.attempted = schedule.acquired = schedule.finished = false;
    schedule.enabled = true;
    std::thread first([&] { require(audio.SetOutputGain(60) == S_OK, "first setter failed"); });
    {
        std::unique_lock<std::mutex> lock(schedule.mutex);
        schedule.wait(lock, [] { return schedule.paused; });
    }
    std::thread second([&] {
        competitor = true;
        HRESULT hr = mode == 0 ? audio.SetOutputGain(120) : audio.SetRuntimeConfig(mode == 1);
        require(hr == S_OK, "competing operation failed");
        std::lock_guard<std::mutex> lock(schedule.mutex);
        schedule.finished = true;
        schedule.cv.notify_all();
    });
    first.join(); second.join();
    schedule.enabled = false;
    const int expected = mode == 0 ? 120 : mode == 1 ? 0 : 60;
    const int persisted = mode == 0 ? 120 : 60;
    const bool passed = !schedule.acquired && audio.m_settings.OpenJocOutputGainTenthsDb == expected &&
        audio.m_openJocOutputGainSnapshot == expected && saved_gain == static_cast<DWORD>(persisted);
    std::cout << (passed ? "PASS" : "FAIL") << " concurrent "
        << (mode == 0 ? "gain setters" : mode == 1 ? "runtime enable" : "persistent reload")
        << ": runtime=" << audio.m_settings.OpenJocOutputGainTenthsDb
        << " saved=" << saved_gain << '\n';
    return passed;
}
int main()
{
    bool passed = true;
    for (int mode = 0; mode < 3; ++mode) passed = concurrent_case(mode) && passed;
    CLAVAudio audio;
    writes = 0; saved_gain = 0;
    require(audio.SetOutputGain(0) == S_OK && writes == 0, "unchanged gain must not write");
    require(audio.SetOutputGain(201) == E_INVALIDARG && writes == 0, "invalid gain must not write");
    audio.m_OutputQueue.openjoc_contract = true;
    audio.flush_result = E_FAIL;
    require(audio.SetOutputGain(60) == E_FAIL && audio.m_openJocOutputGainSnapshot == 0 && writes == 0,
            "failed flush changed gain or registry");
    audio.flush_result = S_OK; audio.reenter = true;
    require(audio.SetOutputGain(60) == S_OK && writes == 1 && saved_gain == 60, "reentrant flush failed");
    require(audio.SetRuntimeConfig(TRUE) == S_OK && audio.SetOutputGain(120) == S_OK && writes == 1,
            "runtime-only gain was persisted");
    require(audio.SetRuntimeConfig(FALSE) == S_OK && audio.m_openJocOutputGainSnapshot == 60,
            "persistent gain was not restored");
    fail_save = true;
    require(FAILED(audio.SetOutputGain(-60)) && audio.m_openJocOutputGainSnapshot == -60 && saved_gain == 60,
            "persistence error semantics changed");
    std::cout << "PASS unchanged/invalid gain, flush failure, recursive readback, runtime-only mode, reload, save failure\n";
    return passed ? 0 : 1;
}
