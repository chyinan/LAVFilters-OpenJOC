/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Isolated COM/settings/registry integration smoke test.

// pattern: Imperative Shell

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <dshow.h>

#include "LAVOpenJocSettings.h"
#include "LAVOpenJocDiagnostics.h"
#include "OpenJocBinauralSettings.h"

namespace
{
constexpr wchar_t kPolicyKey[] = L"Software\\LAV\\Audio\\OpenJOC";
constexpr wchar_t kParentAudioKey[] = L"Software\\LAV\\Audio";
constexpr wchar_t kPolicyVersionValue[] = L"OpenJocOutputPolicyVersion";
constexpr wchar_t kPolicyValue[] = L"OpenJocOutputPolicy";
constexpr wchar_t kDialnormVersionValue[] = L"OpenJocDialnormPolicyVersion";
constexpr wchar_t kDialnormValue[] = L"OpenJocDialnormPolicy";
constexpr wchar_t kBinauralVersionValue[] = L"OpenJocBinauralSettingsVersion";
constexpr wchar_t kBinauralHrtfSourceValue[] = L"OpenJocBinauralHrtfSource";
constexpr wchar_t kBinauralVirtualLayoutValue[] = L"OpenJocBinauralVirtualLayout";
constexpr wchar_t kBinauralSofaPathValue[] = L"OpenJocCustomSofaPath";

constexpr GUID kOpenJocLavAudio = {
    0x27247580, 0xc701, 0x40cd, {0x88, 0x6d, 0xe6, 0x18, 0xfc, 0x8c, 0x9f, 0xff}};
constexpr GUID kOpenJocSettingsIidOracle = {
    0x6b97fd1c, 0xb463, 0x4b5e, {0x93, 0x49, 0xcd, 0x8b, 0x96, 0x4d, 0x6b, 0x46}};
constexpr GUID kOpenJocLevelSettingsIidOracle = {
    0x82fa58e4, 0x10b7, 0x4c25, {0x95, 0xe6, 0x10, 0x98, 0x49, 0x69, 0x95, 0xca}};
constexpr GUID kAudioSettings = {
    0x4158a22b, 0x6553, 0x45d0, {0x80, 0x69, 0x24, 0x71, 0x6f, 0x8f, 0xf1, 0x71}};

// SetRuntimeConfig is the first ILAVAudioSettings member after IUnknown.
struct __declspec(novtable) ITestRuntimeSettings : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetRuntimeConfig(BOOL runtime_config) = 0;
};

template <typename T> void Release(T *&value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

class CurrentUserOverride final
{
  public:
    CurrentUserOverride()
    {
        wchar_t suffix[96] = {};
        _snwprintf_s(suffix, _TRUNCATE, L"Software\\OpenJOC\\Tests\\SettingsSmoke-%lu-%llu",
                     static_cast<unsigned long>(GetCurrentProcessId()),
                     static_cast<unsigned long long>(GetTickCount64()));
        path_ = suffix;

        DWORD disposition = 0;
        status_ = RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                                  KEY_ALL_ACCESS, nullptr, &key_, &disposition);
        if (status_ == ERROR_SUCCESS)
        {
            status_ = RegOverridePredefKey(HKEY_CURRENT_USER, key_);
            overridden_ = status_ == ERROR_SUCCESS;
        }
        if (overridden_)
        {
            HKEY policy_key = nullptr;
            DWORD disposition = 0;
            status_ = RegCreateKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, nullptr, REG_OPTION_VOLATILE,
                                      KEY_ALL_ACCESS, nullptr, &policy_key, &disposition);
            if (policy_key)
                RegCloseKey(policy_key);
            HKEY formats_key = nullptr;
            if (status_ == ERROR_SUCCESS)
                status_ = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\LAV\\Audio\\OpenJOC\\Formats", 0,
                                          nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr,
                                          &formats_key, &disposition);
            if (formats_key)
                RegCloseKey(formats_key);
        }
    }

    ~CurrentUserOverride()
    {
        Restore();
    }

    LONG Restore()
    {
        if (!overridden_)
            return restore_status_;
        restore_status_ = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (restore_status_ != ERROR_SUCCESS)
            return restore_status_;
        overridden_ = false;
        if (key_)
        {
            RegCloseKey(key_);
            key_ = nullptr;
        }
        if (!path_.empty())
            RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        return restore_status_;
    }

    bool ready() const { return overridden_ && status_ == ERROR_SUCCESS; }
    LONG status() const { return status_; }

  private:
    HKEY key_ = nullptr;
    std::wstring path_;
    LONG status_ = ERROR_INVALID_FUNCTION;
    LONG restore_status_ = ERROR_INVALID_FUNCTION;
    bool overridden_ = false;
};

class FilterModule final
{
  public:
    explicit FilterModule(const wchar_t *path)
    {
        std::wstring directory(path);
        const std::size_t separator = directory.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
            SetDllDirectoryW(directory.substr(0, separator).c_str());

        module_ = LoadLibraryW(path);
        if (!module_)
            return;
        auto get_class_object = reinterpret_cast<HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *)>(
            GetProcAddress(module_, "DllGetClassObject"));
        if (get_class_object)
            status_ = get_class_object(kOpenJocLavAudio, IID_IClassFactory,
                                       reinterpret_cast<void **>(&factory_));
    }

    ~FilterModule()
    {
        Release(factory_);
        if (module_)
            FreeLibrary(module_);
        SetDllDirectoryW(nullptr);
    }

    HRESULT Create(ILAVOpenJocSettings **settings, ITestRuntimeSettings **runtime = nullptr) const
    {
        if (!settings)
            return E_POINTER;
        *settings = nullptr;
        if (runtime)
            *runtime = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_FAIL;

        IBaseFilter *filter = nullptr;
        HRESULT hr = factory_->CreateInstance(nullptr, IID_IBaseFilter,
                                              reinterpret_cast<void **>(&filter));
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ILAVOpenJocSettings), reinterpret_cast<void **>(settings));
        if (SUCCEEDED(hr) && runtime)
            hr = filter->QueryInterface(kAudioSettings, reinterpret_cast<void **>(runtime));
        Release(filter);
        if (FAILED(hr))
        {
            Release(*settings);
            if (runtime)
                Release(*runtime);
        }
        return hr;
    }

    HRESULT CreateLevel(ILAVOpenJocLevelSettings **settings, ITestRuntimeSettings **runtime = nullptr) const
    {
        if (!settings)
            return E_POINTER;
        *settings = nullptr;
        if (runtime)
            *runtime = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_FAIL;

        IBaseFilter *filter = nullptr;
        HRESULT hr = factory_->CreateInstance(nullptr, IID_IBaseFilter,
                                              reinterpret_cast<void **>(&filter));
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ILAVOpenJocLevelSettings), reinterpret_cast<void **>(settings));
        if (SUCCEEDED(hr) && runtime)
            hr = filter->QueryInterface(kAudioSettings, reinterpret_cast<void **>(runtime));
        Release(filter);
        if (FAILED(hr))
        {
            Release(*settings);
            if (runtime)
                Release(*runtime);
        }
        return hr;
    }

    HRESULT CreateBinaural(ILAVOpenJocBinauralSettings **settings) const
    {
        if (!settings)
            return E_POINTER;
        *settings = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_FAIL;

        IBaseFilter *filter = nullptr;
        HRESULT hr = factory_->CreateInstance(nullptr, IID_IBaseFilter,
                                              reinterpret_cast<void **>(&filter));
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ILAVOpenJocBinauralSettings),
                                         reinterpret_cast<void **>(settings));
        Release(filter);
        if (FAILED(hr))
            Release(*settings);
        return hr;
    }

    HRESULT CreateDiagnostics(ILAVOpenJocDiagnostics2 **diagnostics) const
    {
        if (!diagnostics)
            return E_POINTER;
        *diagnostics = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_FAIL;

        IBaseFilter *filter = nullptr;
        HRESULT hr = factory_->CreateInstance(nullptr, IID_IBaseFilter,
                                              reinterpret_cast<void **>(&filter));
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ILAVOpenJocDiagnostics2),
                                         reinterpret_cast<void **>(diagnostics));
        Release(filter);
        if (FAILED(hr))
            Release(*diagnostics);
        return hr;
    }

    HRESULT CreateBinauralAndOutput(ILAVOpenJocBinauralSettings **binaural,
                                    ILAVOpenJocSettings **output) const
    {
        if (!binaural || !output)
            return E_POINTER;
        *binaural = nullptr;
        *output = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_FAIL;

        IBaseFilter *filter = nullptr;
        HRESULT hr = factory_->CreateInstance(nullptr, IID_IBaseFilter,
                                              reinterpret_cast<void **>(&filter));
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ILAVOpenJocBinauralSettings),
                                         reinterpret_cast<void **>(binaural));
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                        reinterpret_cast<void **>(output));
        Release(filter);
        if (FAILED(hr))
        {
            Release(*output);
            Release(*binaural);
        }
        return hr;
    }

  private:
    HMODULE module_ = nullptr;
    IClassFactory *factory_ = nullptr;
    HRESULT status_ = E_FAIL;
};

bool ResetPolicyKey()
{
    HKEY key = nullptr;
    const LONG open_status = RegOpenKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, KEY_SET_VALUE, &key);
    if (open_status != ERROR_SUCCESS)
        return false;
    const LONG version_status = RegDeleteValueW(key, kPolicyVersionValue);
    const LONG policy_status = RegDeleteValueW(key, kPolicyValue);
    RegCloseKey(key);
    return (version_status == ERROR_SUCCESS || version_status == ERROR_FILE_NOT_FOUND) &&
           (policy_status == ERROR_SUCCESS || policy_status == ERROR_FILE_NOT_FOUND);
}

bool ResetDialnormKey()
{
    HKEY key = nullptr;
    const LONG open_status = RegOpenKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, KEY_SET_VALUE, &key);
    if (open_status != ERROR_SUCCESS)
        return false;
    const LONG version_status = RegDeleteValueW(key, kDialnormVersionValue);
    const LONG policy_status = RegDeleteValueW(key, kDialnormValue);
    RegCloseKey(key);
    return (version_status == ERROR_SUCCESS || version_status == ERROR_FILE_NOT_FOUND) &&
           (policy_status == ERROR_SUCCESS || policy_status == ERROR_FILE_NOT_FOUND);
}

bool WriteRawValue(const wchar_t *name, const DWORD type, const void *data, const DWORD size)
{
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_SUCCESS)
        status = RegSetValueExW(key, name, 0, type, static_cast<const BYTE *>(data), size);
    if (key)
        RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool WriteDword(const wchar_t *name, const DWORD value)
{
    return WriteRawValue(name, REG_DWORD, &value, sizeof(value));
}

bool ValueIsExactDword(const wchar_t *subkey, const wchar_t *name, const DWORD expected)
{
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key);
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = 0;
    if (status == ERROR_SUCCESS)
        status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
    if (key)
        RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(DWORD) && value == expected;
}

bool ValueIsExactString(const wchar_t *subkey, const wchar_t *name, const std::wstring &expected)
{
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key);
    DWORD type = 0;
    DWORD size = 0;
    if (status == ERROR_SUCCESS)
        status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || type != REG_SZ || size == 0 || size % sizeof(wchar_t) != 0)
    {
        if (key)
            RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(value.data()), &size);
    if (key)
        RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && value.back() == L'\0' &&
           std::wstring(value.data()) == expected;
}

bool ValueIsAbsent(const wchar_t *subkey, const wchar_t *name)
{
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS)
        status = RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr);
    if (key)
        RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

bool ExpectLoadedPolicy(const FilterModule &module, const LAVOpenJocOutputPolicy expected)
{
    ILAVOpenJocSettings *settings = nullptr;
    HRESULT hr = module.Create(&settings);
    LAVOpenJocOutputPolicy actual = LAVOpenJocOutputPolicy::Layout714;
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&actual);
    Release(settings);
    return SUCCEEDED(hr) && actual == expected;
}

bool ResetBinauralKey()
{
    HKEY key = nullptr;
    const LONG open_status = RegOpenKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, KEY_SET_VALUE, &key);
    if (open_status != ERROR_SUCCESS)
        return false;
    const LONG version_status = RegDeleteValueW(key, kBinauralVersionValue);
    const LONG source_status = RegDeleteValueW(key, kBinauralHrtfSourceValue);
    const LONG layout_status = RegDeleteValueW(key, kBinauralVirtualLayoutValue);
    const LONG path_status = RegDeleteValueW(key, kBinauralSofaPathValue);
    RegCloseKey(key);
    return (version_status == ERROR_SUCCESS || version_status == ERROR_FILE_NOT_FOUND) &&
           (source_status == ERROR_SUCCESS || source_status == ERROR_FILE_NOT_FOUND) &&
           (layout_status == ERROR_SUCCESS || layout_status == ERROR_FILE_NOT_FOUND) &&
           (path_status == ERROR_SUCCESS || path_status == ERROR_FILE_NOT_FOUND);
}

bool ExpectLoadedDialnorm(const FilterModule &module, const LAVOpenJocDialnormPolicy expected)
{
    ILAVOpenJocLevelSettings *settings = nullptr;
    HRESULT hr = module.CreateLevel(&settings);
    LAVOpenJocDialnormPolicy actual = LAVOpenJocDialnormPolicy::UnityCompatibility;
    if (SUCCEEDED(hr))
        hr = settings->GetDialnormPolicy(&actual);
    Release(settings);
    return SUCCEEDED(hr) && actual == expected;
}

bool TestDialnormDefaultAndSetters(const FilterModule &module)
{
    if (!ResetDialnormKey())
        return false;

    ILAVOpenJocLevelSettings *settings = nullptr;
    HRESULT hr = module.CreateLevel(&settings);
    LAVOpenJocDialnormPolicy actual = LAVOpenJocDialnormPolicy::UnityCompatibility;
    if (SUCCEEDED(hr))
        hr = settings->GetDialnormPolicy(&actual);
    if (FAILED(hr) || actual != LAVOpenJocDialnormPolicy::Calibrated ||
        settings->GetDialnormPolicy(nullptr) != E_POINTER)
    {
        Release(settings);
        return false;
    }

    for (const auto policy : {LAVOpenJocDialnormPolicy::Calibrated,
                              LAVOpenJocDialnormPolicy::UnityCompatibility})
    {
        if (settings->SetDialnormPolicy(policy) != S_OK ||
            settings->GetDialnormPolicy(&actual) != S_OK || actual != policy)
        {
            Release(settings);
            return false;
        }
    }

    const HRESULT invalid_hr = settings->SetDialnormPolicy(
        static_cast<LAVOpenJocDialnormPolicy>(0xffffffffu));
    const HRESULT get_hr = settings->GetDialnormPolicy(&actual);
    Release(settings);
    return invalid_hr == E_INVALIDARG && get_hr == S_OK &&
           actual == LAVOpenJocDialnormPolicy::UnityCompatibility &&
           ValueIsExactDword(kPolicyKey, kDialnormVersionValue, 1) &&
           ValueIsExactDword(kPolicyKey, kDialnormValue, 1) &&
           ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::UnityCompatibility);
}

bool TestDialnormPersistenceMatrix(const FilterModule &module)
{
    if (!ResetDialnormKey() || !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;
    if (!ResetDialnormKey() || !WriteDword(kDialnormValue, 1) ||
        !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;
    if (!ResetDialnormKey() || !WriteDword(kDialnormVersionValue, 1) ||
        !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;

    struct Case
    {
        DWORD version;
        DWORD policy;
    };
    constexpr Case fallback_cases[] = {{0, 1}, {2, 1}, {1, 2}, {1, 0xffffffffu}};
    for (const auto &test : fallback_cases)
    {
        if (!ResetDialnormKey() || !WriteDword(kDialnormVersionValue, test.version) ||
            !WriteDword(kDialnormValue, test.policy) ||
            !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
            return false;
    }

    const DWORD version = 1;
    const DWORD unity = 1;
    const WORD truncated = 1;
    if (!ResetDialnormKey() ||
        !WriteRawValue(kDialnormVersionValue, REG_BINARY, &version, sizeof(version)) ||
        !WriteDword(kDialnormValue, unity) ||
        !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;
    if (!ResetDialnormKey() || !WriteDword(kDialnormVersionValue, version) ||
        !WriteRawValue(kDialnormValue, REG_BINARY, &unity, sizeof(unity)) ||
        !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;
    if (!ResetDialnormKey() ||
        !WriteRawValue(kDialnormVersionValue, REG_DWORD, &truncated, sizeof(truncated)) ||
        !WriteDword(kDialnormValue, unity) ||
        !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;
    if (!ResetDialnormKey() || !WriteDword(kDialnormVersionValue, version) ||
        !WriteRawValue(kDialnormValue, REG_DWORD, &truncated, sizeof(truncated)) ||
        !ExpectLoadedDialnorm(module, LAVOpenJocDialnormPolicy::Calibrated))
        return false;

    for (const auto policy : {LAVOpenJocDialnormPolicy::Calibrated,
                              LAVOpenJocDialnormPolicy::UnityCompatibility})
    {
        ILAVOpenJocLevelSettings *settings = nullptr;
        HRESULT hr = ResetDialnormKey() ? module.CreateLevel(&settings) : E_FAIL;
        if (SUCCEEDED(hr))
            hr = settings->SetDialnormPolicy(policy);
        Release(settings);
        if (FAILED(hr) ||
            !ValueIsExactDword(kPolicyKey, kDialnormVersionValue, 1) ||
            !ValueIsExactDword(kPolicyKey, kDialnormValue, static_cast<DWORD>(policy)) ||
            !ExpectLoadedDialnorm(module, policy))
            return false;
    }
    return true;
}

bool TestDialnormNamespaceAndRuntimeIsolation(const FilterModule &module)
{
    if (!ResetDialnormKey() || !WriteDword(kDialnormVersionValue, 1) || !WriteDword(kDialnormValue, 0))
        return false;
    ILAVOpenJocLevelSettings *settings = nullptr;
    ITestRuntimeSettings *runtime = nullptr;
    HRESULT hr = module.CreateLevel(&settings, &runtime);
    if (SUCCEEDED(hr))
        hr = runtime->SetRuntimeConfig(TRUE);
    if (SUCCEEDED(hr))
        hr = settings->SetDialnormPolicy(LAVOpenJocDialnormPolicy::UnityCompatibility);
    LAVOpenJocDialnormPolicy actual = LAVOpenJocDialnormPolicy::Calibrated;
    if (SUCCEEDED(hr))
        hr = settings->GetDialnormPolicy(&actual);
    if (SUCCEEDED(hr) && actual != LAVOpenJocDialnormPolicy::UnityCompatibility)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
        hr = runtime->SetRuntimeConfig(FALSE);
    if (SUCCEEDED(hr))
        hr = settings->GetDialnormPolicy(&actual);
    Release(runtime);
    Release(settings);
    return SUCCEEDED(hr) && actual == LAVOpenJocDialnormPolicy::Calibrated &&
           ValueIsExactDword(kPolicyKey, kDialnormVersionValue, 1) &&
           ValueIsExactDword(kPolicyKey, kDialnormValue, 0) &&
           ValueIsAbsent(kParentAudioKey, kDialnormVersionValue) &&
           ValueIsAbsent(kParentAudioKey, kDialnormValue);
}

bool TestDialnormGetterReadbackSourceContract()
{
    std::ifstream source_file(std::filesystem::path("decoder/LAVAudio/LAVAudio.cpp"), std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());
    const std::size_t getter_begin = source.find("HRESULT CLAVAudio::GetDialnormPolicy(");
    const std::size_t getter_end = source.find("HRESULT CLAVAudio::SetDialnormPolicy(", getter_begin);
    if (getter_begin == std::string::npos || getter_end == std::string::npos || getter_begin >= getter_end)
        return false;

    const std::string getter = source.substr(getter_begin, getter_end - getter_begin);
    return getter.find("CAutoLock receive_lock(&m_csReceive)") != std::string::npos &&
           getter.find("CAutoTryLock") == std::string::npos &&
           getter.find("m_settings.OpenJocDialnormPolicy") != std::string::npos &&
           getter.find("return S_OK") != std::string::npos;
}

bool TestDialnormReadbackDuringConcurrentUpdates(const FilterModule &module)
{
    ILAVOpenJocLevelSettings *settings = nullptr;
    ITestRuntimeSettings *runtime = nullptr;
    HRESULT hr = module.CreateLevel(&settings, &runtime);
    if (SUCCEEDED(hr))
        hr = runtime->SetRuntimeConfig(TRUE);
    if (FAILED(hr))
    {
        Release(runtime);
        Release(settings);
        return false;
    }

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int index = 0; index < 10000 && !failed.load(std::memory_order_relaxed); ++index)
        {
            const auto policy = (index & 1) == 0 ? LAVOpenJocDialnormPolicy::Calibrated
                                                 : LAVOpenJocDialnormPolicy::UnityCompatibility;
            if (settings->SetDialnormPolicy(policy) != S_OK)
                failed.store(true, std::memory_order_release);
        }
    });

    start.store(true, std::memory_order_release);
    for (int index = 0; index < 10000 && !failed.load(std::memory_order_relaxed); ++index)
    {
        LAVOpenJocDialnormPolicy actual = LAVOpenJocDialnormPolicy::Calibrated;
        const HRESULT get_hr = settings->GetDialnormPolicy(&actual);
        if (get_hr != S_OK ||
            (actual != LAVOpenJocDialnormPolicy::Calibrated &&
             actual != LAVOpenJocDialnormPolicy::UnityCompatibility))
        {
            failed.store(true, std::memory_order_release);
            break;
        }
    }
    writer.join();
    const HRESULT reset_hr = runtime->SetRuntimeConfig(FALSE);
    Release(runtime);
    Release(settings);
    return SUCCEEDED(reset_hr) && !failed.load(std::memory_order_acquire);
}

bool TestDefaultAndSetters(const FilterModule &module)
{
    if (!ResetPolicyKey())
        return false;

    ILAVOpenJocSettings *settings = nullptr;
    HRESULT hr = module.Create(&settings);
    LAVOpenJocOutputPolicy actual = LAVOpenJocOutputPolicy::Layout714;
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&actual);
    if (FAILED(hr) || actual != LAVOpenJocOutputPolicy::Stereo || settings->GetOutputPolicy(nullptr) != E_POINTER)
    {
        std::fwprintf(stderr, L"default contract detail: hr=0x%08lx policy=%lu settings=%p\n",
                      static_cast<unsigned long>(hr), static_cast<unsigned long>(actual), settings);
        Release(settings);
        return false;
    }

    constexpr LAVOpenJocOutputPolicy policies[] = {
        LAVOpenJocOutputPolicy::Stereo,
        LAVOpenJocOutputPolicy::Binaural,
        LAVOpenJocOutputPolicy::Layout51,
        LAVOpenJocOutputPolicy::Layout71,
        LAVOpenJocOutputPolicy::Layout512,
        LAVOpenJocOutputPolicy::Layout514,
        LAVOpenJocOutputPolicy::Layout712,
        LAVOpenJocOutputPolicy::Layout714,
    };
    for (const auto policy : policies)
    {
        if (settings->SetOutputPolicy(policy) != S_OK ||
            settings->GetOutputPolicy(&actual) != S_OK || actual != policy)
        {
            std::fwprintf(stderr, L"setter contract detail: requested=%lu actual=%lu\n",
                          static_cast<unsigned long>(policy), static_cast<unsigned long>(actual));
            Release(settings);
            return false;
        }
    }

    const HRESULT invalid_hr = settings->SetOutputPolicy(static_cast<LAVOpenJocOutputPolicy>(0xffffffffu));
    const HRESULT get_hr = settings->GetOutputPolicy(&actual);
    Release(settings);
    if (!(invalid_hr == E_INVALIDARG && get_hr == S_OK && actual == LAVOpenJocOutputPolicy::Layout714))
        std::fwprintf(stderr, L"invalid contract detail: set=0x%08lx get=0x%08lx actual=%lu\n",
                      static_cast<unsigned long>(invalid_hr), static_cast<unsigned long>(get_hr),
                      static_cast<unsigned long>(actual));
    return invalid_hr == E_INVALIDARG && get_hr == S_OK && actual == LAVOpenJocOutputPolicy::Layout714 &&
           ValueIsExactDword(kPolicyKey, kPolicyVersionValue, 1) &&
           ValueIsExactDword(kPolicyKey, kPolicyValue, 6) &&
           ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Layout714);
}

bool TestBinauralDefaultsAndInvalidCustom(const FilterModule &module, const wchar_t *valid_sofa_path)
{
    if (!ResetBinauralKey())
        return false;

    ILAVOpenJocBinauralSettings *settings = nullptr;
    HRESULT hr = module.CreateBinaural(&settings);
    LAVOpenJocHrtfSource source = LAVOpenJocHrtfSource::CustomSofa;
    LAVOpenJocBinauralVirtualLayout layout = LAVOpenJocBinauralVirtualLayout::Layout916;
    wchar_t path[32768] = {};
    if (SUCCEEDED(hr))
        hr = settings->GetBinauralHrtfSource(&source);
    if (SUCCEEDED(hr))
        hr = settings->GetBinauralVirtualLayout(&layout);
    if (SUCCEEDED(hr))
        hr = settings->GetCustomSofaPath(path, static_cast<DWORD>(std::size(path)));
    if (FAILED(hr) || source != LAVOpenJocHrtfSource::BuiltinSadieIiD1 ||
        layout != LAVOpenJocBinauralVirtualLayout::Layout714 || path[0] != L'\0')
    {
        Release(settings);
        return false;
    }

    const HRESULT invalid_hr = settings->SetBinauralConfiguration(
        LAVOpenJocOutputPolicy::Binaural, LAVOpenJocHrtfSource::CustomSofa,
        LAVOpenJocBinauralVirtualLayout::Layout714, L"missing-openjoc-test.sofa");
    wchar_t detail[512] = {};
    const HRESULT detail_hr = settings->GetBinauralConfigurationError(
        detail, static_cast<DWORD>(std::size(detail)));
    source = LAVOpenJocHrtfSource::CustomSofa;
    layout = LAVOpenJocBinauralVirtualLayout::Layout916;
    path[0] = L'X';
    if (SUCCEEDED(invalid_hr) || FAILED(detail_hr) || detail[0] == L'\0' ||
        settings->GetBinauralHrtfSource(&source) != S_OK ||
        settings->GetBinauralVirtualLayout(&layout) != S_OK ||
        settings->GetCustomSofaPath(path, static_cast<DWORD>(std::size(path))) != S_OK ||
        source != LAVOpenJocHrtfSource::BuiltinSadieIiD1 ||
        layout != LAVOpenJocBinauralVirtualLayout::Layout714 || path[0] != L'\0' ||
        !ValueIsAbsent(kPolicyKey, kBinauralVersionValue) ||
        !ValueIsAbsent(kPolicyKey, kBinauralHrtfSourceValue) ||
        !ValueIsAbsent(kPolicyKey, kBinauralVirtualLayoutValue) ||
        !ValueIsAbsent(kPolicyKey, kBinauralSofaPathValue))
    {
        Release(settings);
        return false;
    }

    if (valid_sofa_path)
    {
        const HRESULT valid_hr = settings->SetBinauralConfiguration(
                LAVOpenJocOutputPolicy::Binaural, LAVOpenJocHrtfSource::CustomSofa,
                LAVOpenJocBinauralVirtualLayout::Layout714, valid_sofa_path);
        if (valid_hr != S_OK ||
            settings->GetBinauralHrtfSource(&source) != S_OK ||
            source != LAVOpenJocHrtfSource::CustomSofa ||
            settings->GetCustomSofaPath(path, static_cast<DWORD>(std::size(path))) != S_OK ||
            std::wstring(path) != valid_sofa_path)
        {
            wchar_t valid_detail[512] = {};
            settings->GetBinauralConfigurationError(valid_detail, static_cast<DWORD>(std::size(valid_detail)));
            Release(settings);
            return false;
        }
        if (settings->SetBinauralConfiguration(
                LAVOpenJocOutputPolicy::Binaural, LAVOpenJocHrtfSource::CustomSofa,
                LAVOpenJocBinauralVirtualLayout::Layout916, valid_sofa_path) != S_OK ||
            settings->GetBinauralVirtualLayout(&layout) != S_OK ||
            layout != LAVOpenJocBinauralVirtualLayout::Layout916 ||
            !ValueIsExactDword(kPolicyKey, kBinauralVirtualLayoutValue, 1))
        {
            Release(settings);
            return false;
        }
        if (!ValueIsExactDword(kPolicyKey, kBinauralVersionValue, 1) ||
            !ValueIsExactDword(kPolicyKey, kBinauralHrtfSourceValue, 1) ||
            !ValueIsExactDword(kPolicyKey, kBinauralVirtualLayoutValue, 1) ||
            !ValueIsExactString(kPolicyKey, kBinauralSofaPathValue, valid_sofa_path))
        {
            Release(settings);
            return false;
        }
        Release(settings);
        settings = nullptr;
        if (module.CreateBinaural(&settings) != S_OK ||
            settings->GetBinauralHrtfSource(&source) != S_OK ||
            source != LAVOpenJocHrtfSource::CustomSofa ||
            settings->GetBinauralVirtualLayout(&layout) != S_OK ||
            layout != LAVOpenJocBinauralVirtualLayout::Layout916 ||
            settings->GetCustomSofaPath(path, static_cast<DWORD>(std::size(path))) != S_OK ||
            std::wstring(path) != valid_sofa_path)
        {
            std::fwprintf(stderr, L"binaural persistence reload failed\n");
            Release(settings);
            return false;
        }
    }
    Release(settings);
    return ResetBinauralKey();
}

bool TestPersistedMissingCustomIsExplicit(const FilterModule &module)
{
    if (!ResetBinauralKey() || !ResetPolicyKey() ||
        !WriteDword(kBinauralVersionValue, 1) || !WriteDword(kBinauralHrtfSourceValue, 1) ||
        !WriteDword(kBinauralVirtualLayoutValue, 0) ||
        !WriteRawValue(kBinauralSofaPathValue, REG_SZ, L"missing-persisted.sofa",
                       static_cast<DWORD>(sizeof(L"missing-persisted.sofa"))) ||
        !WriteDword(kPolicyVersionValue, 1) || !WriteDword(kPolicyValue, 7))
        return false;

    ILAVOpenJocBinauralSettings *binaural = nullptr;
    ILAVOpenJocSettings *output = nullptr;
    ILAVOpenJocDiagnostics2 *diagnostics = nullptr;
    HRESULT hr = module.CreateBinaural(&binaural);
    if (SUCCEEDED(hr))
        hr = module.Create(&output);
    if (SUCCEEDED(hr))
        hr = module.CreateDiagnostics(&diagnostics);
    LAVOpenJocHrtfSource source = LAVOpenJocHrtfSource::BuiltinSadieIiD1;
    LAVOpenJocBinauralVirtualLayout layout = LAVOpenJocBinauralVirtualLayout::Layout714;
    LAVOpenJocOutputPolicy policy = LAVOpenJocOutputPolicy::Stereo;
    wchar_t detail[512] = {};
    wchar_t path[32768] = {};
    LAVOpenJocDiagnosticReason reason = LAVOpenJocDiagnosticNone;
    BOOL warning = FALSE;
    BOOL failure_au_known = TRUE;
    ULONGLONG failure_au = 0;
    if (SUCCEEDED(hr))
        hr = binaural->GetBinauralHrtfSource(&source);
    if (SUCCEEDED(hr))
        hr = binaural->GetBinauralVirtualLayout(&layout);
    if (SUCCEEDED(hr))
        hr = binaural->GetCustomSofaPath(path, static_cast<DWORD>(std::size(path)));
    if (SUCCEEDED(hr))
        hr = binaural->GetBinauralConfigurationError(detail, static_cast<DWORD>(std::size(detail)));
    if (SUCCEEDED(hr))
        hr = diagnostics->GetOpenJocPlaybackDiagnostics(&reason, &warning, &failure_au_known, &failure_au,
                                                        detail, static_cast<DWORD>(std::size(detail)));
    if (SUCCEEDED(hr))
        hr = output->GetOutputPolicy(&policy);
    const bool passed = SUCCEEDED(hr) && source == LAVOpenJocHrtfSource::CustomSofa &&
                        layout == LAVOpenJocBinauralVirtualLayout::Layout714 &&
                        std::wstring(path) == L"missing-persisted.sofa" &&
                        std::wstring(detail).find(L"could not be opened") != std::wstring::npos &&
                        reason == LAVOpenJocDiagnosticBinauralHrtfConfiguration && warning == TRUE &&
                        failure_au_known == FALSE && failure_au == 0 &&
                        policy == LAVOpenJocOutputPolicy::Binaural;
    Release(diagnostics);
    Release(output);
    Release(binaural);
    return ResetBinauralKey() && ResetPolicyKey() && passed;
}

bool TestStructurallyInvalidBinauralSettingsUseSafeDefaults(const FilterModule &module)
{
    if (!ResetBinauralKey() || !ResetPolicyKey() ||
        !WriteDword(kBinauralVersionValue, 1) || !WriteDword(kBinauralHrtfSourceValue, 99) ||
        !WriteDword(kBinauralVirtualLayoutValue, 99) || !WriteDword(kPolicyVersionValue, 1) ||
        !WriteDword(kPolicyValue, 7))
        return false;

    ILAVOpenJocBinauralSettings *settings = nullptr;
    ILAVOpenJocSettings *output = nullptr;
    HRESULT hr = module.CreateBinaural(&settings);
    if (SUCCEEDED(hr))
        hr = module.Create(&output);
    LAVOpenJocHrtfSource source = LAVOpenJocHrtfSource::CustomSofa;
    LAVOpenJocBinauralVirtualLayout layout = LAVOpenJocBinauralVirtualLayout::Layout916;
    LAVOpenJocOutputPolicy policy = LAVOpenJocOutputPolicy::Stereo;
    wchar_t detail[512] = {};
    if (SUCCEEDED(hr))
        hr = settings->GetBinauralHrtfSource(&source);
    if (SUCCEEDED(hr))
        hr = settings->GetBinauralVirtualLayout(&layout);
    if (SUCCEEDED(hr))
        hr = settings->GetBinauralConfigurationError(detail, static_cast<DWORD>(std::size(detail)));
    if (SUCCEEDED(hr))
        hr = output->GetOutputPolicy(&policy);
    const bool passed = SUCCEEDED(hr) && source == LAVOpenJocHrtfSource::BuiltinSadieIiD1 &&
                        layout == LAVOpenJocBinauralVirtualLayout::Layout714 && detail[0] == L'\0' &&
                        policy == LAVOpenJocOutputPolicy::Binaural;
    Release(output);
    Release(settings);
    return ResetBinauralKey() && ResetPolicyKey() && passed;
}

bool TestNonBinauralApplyDoesNotLoadCustomSofa(const FilterModule &module)
{
    if (!ResetBinauralKey() || !ResetPolicyKey())
        return false;
    ILAVOpenJocBinauralSettings *binaural = nullptr;
    ILAVOpenJocSettings *output = nullptr;
    HRESULT hr = module.CreateBinauralAndOutput(&binaural, &output);
    if (SUCCEEDED(hr))
        hr = binaural->SetBinauralConfiguration(
            LAVOpenJocOutputPolicy::Stereo, LAVOpenJocHrtfSource::CustomSofa,
            LAVOpenJocBinauralVirtualLayout::Layout714, L"missing-while-stereo.sofa");
    LAVOpenJocOutputPolicy policy = LAVOpenJocOutputPolicy::Binaural;
    if (SUCCEEDED(hr))
        hr = output->GetOutputPolicy(&policy);
    const HRESULT binal_switch_hr = SUCCEEDED(hr) ? output->SetOutputPolicy(LAVOpenJocOutputPolicy::Binaural) : E_FAIL;
    wchar_t detail[512] = {};
    if (SUCCEEDED(hr))
        hr = binaural->GetBinauralConfigurationError(detail, static_cast<DWORD>(std::size(detail)));
    const bool passed = SUCCEEDED(hr) && policy == LAVOpenJocOutputPolicy::Stereo &&
                        FAILED(binal_switch_hr) &&
                        std::wstring(detail).find(L"could not be opened") != std::wstring::npos;
    Release(output);
    Release(binaural);
    return ResetBinauralKey() && ResetPolicyKey() && passed;
}

bool TestPersistenceMatrix(const FilterModule &module)
{
    if (!ResetPolicyKey() || !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;
    if (!ResetPolicyKey() || !WriteDword(kPolicyValue, 6) ||
        !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;
    if (!ResetPolicyKey() || !WriteDword(kPolicyVersionValue, 1) ||
        !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;

    struct Case
    {
        DWORD version;
        DWORD policy;
    };
    constexpr Case fallback_cases[] = {{0, 6}, {2, 6}, {1, 8}, {1, 0xffffffffu}};
    for (const auto &test : fallback_cases)
    {
        if (!ResetPolicyKey() || !WriteDword(kPolicyVersionValue, test.version) ||
            !WriteDword(kPolicyValue, test.policy) ||
            !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
            return false;
    }

    const DWORD version = 1;
    const DWORD policy = 6;
    const WORD truncated = 1;
    if (!ResetPolicyKey() || !WriteRawValue(kPolicyVersionValue, REG_BINARY, &version, sizeof(version)) ||
        !WriteDword(kPolicyValue, policy) || !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;
    if (!ResetPolicyKey() || !WriteDword(kPolicyVersionValue, version) ||
        !WriteRawValue(kPolicyValue, REG_BINARY, &policy, sizeof(policy)) ||
        !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;
    if (!ResetPolicyKey() || !WriteRawValue(kPolicyVersionValue, REG_DWORD, &truncated, sizeof(truncated)) ||
        !WriteDword(kPolicyValue, policy) || !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;
    if (!ResetPolicyKey() || !WriteDword(kPolicyVersionValue, version) ||
        !WriteRawValue(kPolicyValue, REG_DWORD, &truncated, sizeof(truncated)) ||
        !ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Stereo))
        return false;

    return ResetPolicyKey() && WriteDword(kPolicyVersionValue, version) && WriteDword(kPolicyValue, policy) &&
           ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Layout714);
}

bool TestRoundTripAndIsolation(const FilterModule &module)
{
    if (!ResetPolicyKey())
        return false;
    ILAVOpenJocSettings *settings = nullptr;
    HRESULT hr = module.Create(&settings);
    if (SUCCEEDED(hr))
        hr = settings->SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714);
    Release(settings);
    if (FAILED(hr) || !ValueIsExactDword(kPolicyKey, kPolicyVersionValue, 1) ||
        !ValueIsExactDword(kPolicyKey, kPolicyValue, 6) ||
        !ValueIsAbsent(kParentAudioKey, kPolicyVersionValue) || !ValueIsAbsent(kParentAudioKey, kPolicyValue))
        return false;
    return ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Layout714);
}

bool TestRuntimeConfigDoesNotWrite(const FilterModule &module)
{
    if (!ResetPolicyKey() || !WriteDword(kPolicyVersionValue, 1) || !WriteDword(kPolicyValue, 6))
        return false;
    ILAVOpenJocSettings *settings = nullptr;
    ITestRuntimeSettings *runtime = nullptr;
    HRESULT hr = module.Create(&settings, &runtime);
    LAVOpenJocOutputPolicy actual = LAVOpenJocOutputPolicy::Stereo;
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&actual);
    if (SUCCEEDED(hr) && actual != LAVOpenJocOutputPolicy::Layout714)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
        hr = runtime->SetRuntimeConfig(TRUE);
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&actual);
    if (SUCCEEDED(hr) && actual != LAVOpenJocOutputPolicy::Stereo)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
        hr = settings->SetOutputPolicy(LAVOpenJocOutputPolicy::Layout514);
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&actual);
    if (SUCCEEDED(hr) && actual != LAVOpenJocOutputPolicy::Layout514)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
        hr = runtime->SetRuntimeConfig(FALSE);
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&actual);
    Release(runtime);
    Release(settings);
    return SUCCEEDED(hr) && actual == LAVOpenJocOutputPolicy::Layout714 &&
           ValueIsExactDword(kPolicyKey, kPolicyVersionValue, 1) &&
           ValueIsExactDword(kPolicyKey, kPolicyValue, 6) &&
           ExpectLoadedPolicy(module, LAVOpenJocOutputPolicy::Layout714);
}

bool TestPolicyReloadClearsIncompatibleQueues()
{
    const std::filesystem::path source_path = std::filesystem::path(__FILE__).parent_path() / "LAVAudio.cpp";
    std::ifstream source_file(source_path, std::ios::binary);
    if (!source_file.good())
        return false;
    const std::string source((std::istreambuf_iterator<char>(source_file)), std::istreambuf_iterator<char>());

    const std::size_t load_begin = source.find("HRESULT CLAVAudio::LoadSettings()");
    const std::size_t load_end = source.find("HRESULT CLAVAudio::LoadOpenJocOutputPolicySettings()", load_begin);
    if (load_begin == std::string::npos || load_end == std::string::npos)
        return false;
    const std::string load = source.substr(load_begin, load_end - load_begin);
    if (load.find("ConfigureOpenJocOutputPolicy(m_settings.OpenJocOutputPolicy, true)") == std::string::npos ||
        load.find("ConfigureOpenJocOutputPolicy(LAVOpenJocOutputPolicy::Stereo, true)") == std::string::npos ||
        load.find("ConfigureOpenJocDialnormPolicy(m_settings.OpenJocDialnormPolicy, true)") == std::string::npos ||
        load.find("ConfigureOpenJocDialnormPolicy(LAVOpenJocDialnormPolicy::Calibrated, true)") == std::string::npos)
        return false;

    const std::size_t configure_begin = source.find("HRESULT CLAVAudio::ConfigureOpenJocOutputPolicy(");
    const std::size_t configure_end = source.find("HRESULT CLAVAudio::ReadSettings(", configure_begin);
    if (configure_begin == std::string::npos || configure_end == std::string::npos)
        return false;
    const std::string configure = source.substr(configure_begin, configure_end - configure_begin);
    const std::size_t decoder_transition = configure.find("m_openJoc.SetOutputPolicy(policy)");
    const std::size_t changed = configure.find("if (changed && clear_queues)");
    const std::size_t clear_input = configure.find("m_buff.Clear();", changed);
    const std::size_t clear_output = configure.find("FlushOutput(FALSE);", clear_input);
    const std::size_t queue_resync = configure.find("m_bQueueResync = TRUE;", clear_output);
    const std::size_t timestamp_resync = configure.find("m_bResyncTimestamp = FALSE;", queue_resync);
    if (decoder_transition == std::string::npos || changed == std::string::npos ||
        clear_input == std::string::npos || clear_output == std::string::npos ||
        queue_resync == std::string::npos || timestamp_resync == std::string::npos ||
        !(decoder_transition < changed && changed < clear_input && clear_input < clear_output &&
          clear_output < queue_resync && queue_resync < timestamp_resync))
        return false;

    const std::size_t dialnorm_configure_begin = source.find("HRESULT CLAVAudio::ConfigureOpenJocDialnormPolicy(");
    const std::size_t dialnorm_configure_end = source.find("HRESULT CLAVAudio::ReadSettings(", dialnorm_configure_begin);
    if (dialnorm_configure_begin == std::string::npos || dialnorm_configure_end == std::string::npos)
        return false;
    const std::string dialnorm_configure =
        source.substr(dialnorm_configure_begin, dialnorm_configure_end - dialnorm_configure_begin);
    const std::size_t dialnorm_transition = dialnorm_configure.find("m_openJoc.SetDialnormPolicy(policy)");
    const std::size_t dialnorm_changed = dialnorm_configure.find("if (changed && clear_queues)");
    const std::size_t dialnorm_clear_input = dialnorm_configure.find("m_buff.Clear();", dialnorm_changed);
    const std::size_t dialnorm_clear_output = dialnorm_configure.find("FlushOutput(FALSE);", dialnorm_clear_input);
    if (dialnorm_transition == std::string::npos || dialnorm_changed == std::string::npos ||
        dialnorm_clear_input == std::string::npos || dialnorm_clear_output == std::string::npos ||
        !(dialnorm_transition < dialnorm_changed && dialnorm_changed < dialnorm_clear_input &&
          dialnorm_clear_input < dialnorm_clear_output))
        return false;

    const std::size_t flush_begin = source.find("HRESULT CLAVAudio::FlushOutput(BOOL bDeliver)");
    const std::size_t flush_end = source.find("static HRESULT CreateOpenJocStrictDirectShowMediaType", flush_begin);
    if (flush_begin == std::string::npos || flush_end == std::string::npos)
        return false;
    const std::string flush = source.substr(flush_begin, flush_end - flush_begin);
    return flush.find("m_OutputQueue.nSamples = 0;") != std::string::npos &&
           flush.find("m_OutputQueue.bBuffer->SetSize(0);") != std::string::npos &&
           flush.find("m_OutputQueue.rtStart = AV_NOPTS_VALUE;") != std::string::npos &&
           flush.find("m_OutputQueue.openjoc_contract = nullptr;") != std::string::npos;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    static_assert(sizeof(LAVOpenJocOutputPolicy) == sizeof(std::uint32_t));
    static_assert(sizeof(LAVOpenJocDialnormPolicy) == sizeof(std::uint32_t));
    if (!IsEqualGUID(__uuidof(ILAVOpenJocSettings), kOpenJocSettingsIidOracle))
    {
        std::fwprintf(stderr, L"ILAVOpenJocSettings IID oracle mismatch\n");
        return 1;
    }
    if (!IsEqualGUID(__uuidof(ILAVOpenJocLevelSettings), kOpenJocLevelSettingsIidOracle))
    {
        std::fwprintf(stderr, L"ILAVOpenJocLevelSettings IID oracle mismatch\n");
        return 1;
    }
    if (argc != 2 && argc != 3)
    {
        std::fwprintf(stderr, L"usage: OpenJocSettingsSmoke.exe <OpenJOC LAVAudio.ax> [valid SOFA path]\n");
        return 2;
    }
    if (!TestPolicyReloadClearsIncompatibleQueues())
    {
        std::fwprintf(stderr, L"policy reload queue-reset source contract failed\n");
        return 1;
    }
    if (!TestDialnormGetterReadbackSourceContract())
    {
        std::fwprintf(stderr, L"dialnorm getter readback source contract failed\n");
        return 1;
    }

    CurrentUserOverride registry_override;
    if (!registry_override.ready())
    {
        std::fwprintf(stderr, L"RegOverridePredefKey setup failed: %ld\n", registry_override.status());
        return 1;
    }

    int test_result = 0;
    {
        FilterModule module(argv[1]);
        if (!TestBinauralDefaultsAndInvalidCustom(module, argc == 3 ? argv[2] : nullptr))
        {
            std::fwprintf(stderr, L"binaural settings contract failed\n");
            test_result = 1;
        }
        else if (!TestPersistedMissingCustomIsExplicit(module))
        {
            std::fwprintf(stderr, L"persisted missing custom SOFA contract failed\n");
            test_result = 1;
        }
        else if (!TestStructurallyInvalidBinauralSettingsUseSafeDefaults(module))
        {
            std::fwprintf(stderr, L"invalid binaural settings fallback contract failed\n");
            test_result = 1;
        }
        else if (!TestNonBinauralApplyDoesNotLoadCustomSofa(module))
        {
            std::fwprintf(stderr, L"non-binaural custom SOFA isolation contract failed\n");
            test_result = 1;
        }
        else if (!TestDefaultAndSetters(module))
        {
            std::fwprintf(stderr, L"default/setter policy contract failed\n");
            test_result = 1;
        }
        else if (!TestPersistenceMatrix(module))
        {
            std::fwprintf(stderr, L"strict registry policy matrix failed\n");
            test_result = 1;
        }
        else if (!TestRoundTripAndIsolation(module))
        {
            std::fwprintf(stderr, L"policy round-trip/isolation failed\n");
            test_result = 1;
        }
        else if (!TestRuntimeConfigDoesNotWrite(module))
        {
            std::fwprintf(stderr, L"runtime-config registry isolation failed\n");
            test_result = 1;
        }
        else if (!TestDialnormDefaultAndSetters(module))
        {
            std::fwprintf(stderr, L"default/setter dialnorm contract failed\n");
            test_result = 1;
        }
        else if (!TestDialnormPersistenceMatrix(module))
        {
            std::fwprintf(stderr, L"strict registry dialnorm matrix failed\n");
            test_result = 1;
        }
        else if (!TestDialnormNamespaceAndRuntimeIsolation(module))
        {
            std::fwprintf(stderr, L"dialnorm namespace/runtime isolation failed\n");
            test_result = 1;
        }
        else if (!TestDialnormReadbackDuringConcurrentUpdates(module))
        {
            std::fwprintf(stderr, L"dialnorm readback during concurrent updates failed\n");
            test_result = 1;
        }
    }

    if (registry_override.Restore() != ERROR_SUCCESS)
    {
        std::fwprintf(stderr, L"RegOverridePredefKey restore failed\n");
        return 1;
    }
    if (test_result != 0)
        return test_result;

    std::wprintf(L"OpenJOC settings smoke passed\n");
    return 0;
}
