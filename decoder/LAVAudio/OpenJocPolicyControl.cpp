/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#include <windows.h>

#include <dshow.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "LAVAudioSettings.h"
#include "LAVOpenJocSettings.h"

namespace
{
constexpr GUID kTargetLavAudio = {
    0x27247580, 0xc701, 0x40cd, {0x88, 0x6d, 0xe6, 0x18, 0xfc, 0x8c, 0x9f, 0xff}};
constexpr wchar_t kPolicyKey[] = L"Software\\LAV\\Audio\\OpenJOC";
constexpr wchar_t kPolicyVersionValue[] = L"OpenJocOutputPolicyVersion";
constexpr wchar_t kPolicyValue[] = L"OpenJocOutputPolicy";

template <typename T> void Release(T *&value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

std::wstring FinalPathForFile(const std::filesystem::path &path)
{
    if (!path.is_absolute())
        return {};
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    const DWORD required = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
    std::wstring result;
    if (required != 0)
    {
        result.resize(required);
        const DWORD copied =
            GetFinalPathNameByHandleW(file, result.data(), required, FILE_NAME_NORMALIZED);
        if (copied == 0 || copied >= required)
            result.clear();
        else
            result.resize(copied);
    }
    CloseHandle(file);
    return result;
}

std::wstring ModulePath(const HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD copied = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (copied == 0 || copied >= path.size())
        return {};
    path.resize(copied);
    return FinalPathForFile(path);
}

bool SamePath(const std::wstring &left, const std::wstring &right)
{
    return !left.empty() && left.size() == right.size() &&
           _wcsicmp(left.c_str(), right.c_str()) == 0;
}

class ScopedActivationContext final
{
  public:
    ScopedActivationContext(const std::wstring &module_path,
                            const std::wstring &assembly_directory)
    {
        ACTCTXW configuration{};
        configuration.cbSize = sizeof(configuration);
        configuration.dwFlags = ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID |
                                ACTCTX_FLAG_RESOURCE_NAME_VALID;
        configuration.lpSource = module_path.c_str();
        configuration.lpAssemblyDirectory = assembly_directory.c_str();
        configuration.lpResourceName = MAKEINTRESOURCEW(2);
        context_ = CreateActCtxW(&configuration);
        if (context_ != INVALID_HANDLE_VALUE && !ActivateActCtx(context_, &cookie_))
        {
            ReleaseActCtx(context_);
            context_ = INVALID_HANDLE_VALUE;
            cookie_ = 0;
        }
    }

    ~ScopedActivationContext()
    {
        if (cookie_)
            DeactivateActCtx(0, cookie_);
        if (context_ != INVALID_HANDLE_VALUE)
            ReleaseActCtx(context_);
    }

    ScopedActivationContext(const ScopedActivationContext &) = delete;
    ScopedActivationContext &operator=(const ScopedActivationContext &) = delete;
    bool active() const { return context_ != INVALID_HANDLE_VALUE && cookie_ != 0; }

  private:
    HANDLE context_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
};

class PrivateComModule final
{
  public:
    explicit PrivateComModule(const std::filesystem::path &absolute_module_path)
        : path_(FinalPathForFile(absolute_module_path))
    {
        if (path_.empty())
        {
            status_ = E_INVALIDARG;
            return;
        }
        activation_ = new (std::nothrow) ScopedActivationContext(
            path_, std::filesystem::path(path_).parent_path().native());
        if (!activation_ || !activation_->active())
        {
            status_ = HRESULT_FROM_WIN32(GetLastError());
            return;
        }
        module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module_)
        {
            status_ = HRESULT_FROM_WIN32(GetLastError());
            return;
        }
        if (!SamePath(path_, ModulePath(module_)))
        {
            status_ = E_UNEXPECTED;
            return;
        }
        using DllGetClassObjectProc = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);
        const auto get_class_object = reinterpret_cast<DllGetClassObjectProc>(
            GetProcAddress(module_, "DllGetClassObject"));
        if (!get_class_object)
        {
            status_ = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            return;
        }
        status_ = get_class_object(kTargetLavAudio, IID_IClassFactory,
                                   reinterpret_cast<void **>(&factory_));
    }

    ~PrivateComModule()
    {
        Release(factory_);
        if (module_)
            FreeLibrary(module_);
        delete activation_;
    }

    PrivateComModule(const PrivateComModule &) = delete;
    PrivateComModule &operator=(const PrivateComModule &) = delete;

    HRESULT status() const { return status_; }
    const std::wstring &path() const { return path_; }

    HRESULT Create(IBaseFilter **filter) const
    {
        if (!filter)
            return E_POINTER;
        *filter = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_UNEXPECTED;
        return factory_->CreateInstance(nullptr, IID_IBaseFilter,
                                        reinterpret_cast<void **>(filter));
    }

  private:
    std::wstring path_;
    HMODULE module_ = nullptr;
    IClassFactory *factory_ = nullptr;
    ScopedActivationContext *activation_ = nullptr;
    HRESULT status_ = E_FAIL;
};

struct RegistryValue
{
    LONG status = ERROR_INVALID_FUNCTION;
    DWORD type = 0;
    DWORD size = 0;
    DWORD value = 0;
};

RegistryValue ReadRegistryValue(const wchar_t *name)
{
    RegistryValue result;
    HKEY key = nullptr;
    result.status = RegOpenKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, KEY_QUERY_VALUE, &key);
    if (result.status == ERROR_SUCCESS)
    {
        result.size = sizeof(result.value);
        result.status = RegQueryValueExW(key, name, nullptr, &result.type,
                                         reinterpret_cast<BYTE *>(&result.value), &result.size);
    }
    if (key)
        RegCloseKey(key);
    return result;
}

bool IsAbsent(const RegistryValue &value)
{
    return value.status == ERROR_FILE_NOT_FOUND || value.status == ERROR_PATH_NOT_FOUND;
}

bool IsExactDword(const RegistryValue &value, const DWORD expected)
{
    return value.status == ERROR_SUCCESS && value.type == REG_DWORD &&
           value.size == sizeof(DWORD) && value.value == expected;
}

struct RawRegistryValue
{
    LONG status = ERROR_INVALID_FUNCTION;
    DWORD type = 0;
    std::vector<BYTE> data;

    bool operator==(const RawRegistryValue &right) const
    {
        return status == right.status && type == right.type && data == right.data;
    }
};

using PolicyRegistrySnapshot = std::array<RawRegistryValue, 2>;

RawRegistryValue SnapshotRegistryValue(const wchar_t *name)
{
    RawRegistryValue result;
    HKEY key = nullptr;
    result.status = RegOpenKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, KEY_QUERY_VALUE, &key);
    if (result.status == ERROR_SUCCESS)
    {
        DWORD size = 0;
        result.status = RegQueryValueExW(key, name, nullptr, &result.type, nullptr, &size);
        if (result.status == ERROR_SUCCESS)
        {
            result.data.resize(size);
            DWORD actual_size = size;
            result.status = RegQueryValueExW(key, name, nullptr, &result.type,
                                             result.data.empty() ? nullptr : result.data.data(),
                                             &actual_size);
            if (result.status == ERROR_SUCCESS)
                result.data.resize(actual_size);
        }
        RegCloseKey(key);
    }
    return result;
}

PolicyRegistrySnapshot SnapshotPolicyRegistry()
{
    return {SnapshotRegistryValue(kPolicyVersionValue), SnapshotRegistryValue(kPolicyValue)};
}

HRESULT ReadInstancePolicy(const PrivateComModule &module, LAVOpenJocOutputPolicy *policy)
{
    if (!policy)
        return E_POINTER;
    IBaseFilter *filter = nullptr;
    ILAVOpenJocSettings *settings = nullptr;
    HRESULT status = module.Create(&filter);
    if (SUCCEEDED(status))
        status = filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                        reinterpret_cast<void **>(&settings));
    if (SUCCEEDED(status))
        status = settings->GetOutputPolicy(policy);
    Release(settings);
    Release(filter);
    return status;
}

bool PolicyStateMatchesExpected(const PrivateComModule &module,
                                const LAVOpenJocOutputPolicy expected)
{
    LAVOpenJocOutputPolicy instance = LAVOpenJocOutputPolicy::Stereo;
    const HRESULT get_status = ReadInstancePolicy(module, &instance);
    const RegistryValue version = ReadRegistryValue(kPolicyVersionValue);
    const RegistryValue policy = ReadRegistryValue(kPolicyValue);
    return get_status == S_OK && instance == expected &&
           IsExactDword(version, LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION) &&
           IsExactDword(policy, static_cast<DWORD>(expected));
}

bool ReportAndVerify(const PrivateComModule &module, const wchar_t *operation,
                     const LAVOpenJocOutputPolicy *expected)
{
    LAVOpenJocOutputPolicy instance = LAVOpenJocOutputPolicy::Stereo;
    const HRESULT get_status = ReadInstancePolicy(module, &instance);
    const RegistryValue version = ReadRegistryValue(kPolicyVersionValue);
    const RegistryValue policy = ReadRegistryValue(kPolicyValue);
    const bool absent_default = IsAbsent(version) && IsAbsent(policy) &&
                                instance == LAVOpenJocOutputPolicy::Stereo;
    const bool exact_persistent =
        IsExactDword(version, LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION) &&
        policy.status == ERROR_SUCCESS && policy.type == REG_DWORD &&
        policy.size == sizeof(DWORD) && policy.value < LAV_OPENJOC_OUTPUT_CONTRACT_COUNT &&
        static_cast<std::uint32_t>(instance) == policy.value;
    const bool expected_persistent =
        expected && get_status == S_OK && instance == *expected &&
        IsExactDword(version, LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION) &&
        IsExactDword(policy, static_cast<DWORD>(*expected));
    const bool verified = expected ? expected_persistent
                                   : get_status == S_OK && (absent_default || exact_persistent);
    std::wprintf(
        L"POLICY_CONTROL operation=%ls module=\"%ls\" get_hr=0x%08lx instance_policy=%lu "
        L"registry_version_status=%ld registry_version_type=%lu registry_version_size=%lu "
        L"registry_version=%lu registry_policy_status=%ld registry_policy_type=%lu "
        L"registry_policy_size=%lu registry_policy=%lu expected_policy=%ld verified=%d\n",
        operation, module.path().c_str(), static_cast<unsigned long>(get_status),
        static_cast<unsigned long>(instance), version.status,
        static_cast<unsigned long>(version.type), static_cast<unsigned long>(version.size),
        static_cast<unsigned long>(version.value), policy.status,
        static_cast<unsigned long>(policy.type), static_cast<unsigned long>(policy.size),
        static_cast<unsigned long>(policy.value),
        expected ? static_cast<long>(*expected) : -1L, verified ? 1 : 0);
    return verified;
}

HRESULT SetPersistent(const PrivateComModule &module, const LAVOpenJocOutputPolicy policy)
{
    IBaseFilter *filter = nullptr;
    ILAVAudioSettings *audio_settings = nullptr;
    ILAVOpenJocSettings *openjoc_settings = nullptr;
    HRESULT status = module.Create(&filter);
    if (SUCCEEDED(status))
        status = filter->QueryInterface(__uuidof(ILAVAudioSettings),
                                        reinterpret_cast<void **>(&audio_settings));
    if (SUCCEEDED(status))
        status = filter->QueryInterface(__uuidof(ILAVOpenJocSettings),
                                        reinterpret_cast<void **>(&openjoc_settings));
    if (SUCCEEDED(status))
        status = audio_settings->SetRuntimeConfig(FALSE);
    if (SUCCEEDED(status))
        status = openjoc_settings->SetOutputPolicy(policy);
    Release(openjoc_settings);
    Release(audio_settings);
    Release(filter);
    return status;
}

bool ParsePolicy(const wchar_t *text, LAVOpenJocOutputPolicy *policy)
{
    if (!text || !*text || !policy)
        return false;
    wchar_t *end = nullptr;
    const unsigned long value = wcstoul(text, &end, 10);
    if (!end || *end != L'\0' || value >= LAV_OPENJOC_OUTPUT_CONTRACT_COUNT)
        return false;
    *policy = static_cast<LAVOpenJocOutputPolicy>(value);
    return true;
}

class CurrentUserOverride final
{
  public:
    CurrentUserOverride()
    {
        wchar_t suffix[96] = {};
        _snwprintf_s(suffix, _TRUNCATE, L"Software\\OpenJOC\\Tests\\PolicyControl-%lu-%llu",
                     static_cast<unsigned long>(GetCurrentProcessId()),
                     static_cast<unsigned long long>(GetTickCount64()));
        path_ = suffix;
        DWORD disposition = 0;
        status_ = RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr,
                                  REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &key_,
                                  &disposition);
        if (status_ == ERROR_SUCCESS)
            status_ = RegOverridePredefKey(HKEY_CURRENT_USER, key_);
        overridden_ = status_ == ERROR_SUCCESS;
        if (overridden_)
        {
            HKEY policy_key = nullptr;
            status_ = RegCreateKeyExW(HKEY_CURRENT_USER, kPolicyKey, 0, nullptr,
                                      REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &policy_key,
                                      &disposition);
            if (policy_key)
                RegCloseKey(policy_key);
        }
    }

    ~CurrentUserOverride() { Restore(); }

    bool ready() const { return overridden_ && status_ == ERROR_SUCCESS; }
    LONG status() const { return status_; }

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
        const LONG delete_status = RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        if (delete_status != ERROR_SUCCESS && delete_status != ERROR_FILE_NOT_FOUND &&
            delete_status != ERROR_PATH_NOT_FOUND)
        {
            restore_status_ = delete_status;
            return restore_status_;
        }
        HKEY remaining = nullptr;
        const LONG open_status = RegOpenKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0,
                                               KEY_QUERY_VALUE, &remaining);
        if (remaining)
            RegCloseKey(remaining);
        temporary_tree_absent_ =
            open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND;
        if (!temporary_tree_absent_)
            restore_status_ = open_status == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : open_status;
        return restore_status_;
    }

    bool temporary_tree_absent() const { return temporary_tree_absent_; }

  private:
    HKEY key_ = nullptr;
    std::wstring path_;
    LONG status_ = ERROR_INVALID_FUNCTION;
    LONG restore_status_ = ERROR_INVALID_FUNCTION;
    bool overridden_ = false;
    bool temporary_tree_absent_ = false;
};

int RunSelfTest(const wchar_t *module_path)
{
    const PolicyRegistrySnapshot real_before = SnapshotPolicyRegistry();
    CurrentUserOverride registry;
    if (!registry.ready())
    {
        std::fwprintf(stderr, L"isolated registry setup failed: %ld\n", registry.status());
        return 1;
    }
    int result = 0;
    {
        PrivateComModule module(module_path);
        if (FAILED(module.status()))
        {
            std::fwprintf(stderr, L"private target activation failed: 0x%08lx\n",
                          static_cast<unsigned long>(module.status()));
            result = 1;
        }
        for (std::uint32_t value = 0; result == 0 && value < LAV_OPENJOC_OUTPUT_CONTRACT_COUNT; ++value)
        {
            const auto policy = static_cast<LAVOpenJocOutputPolicy>(value);
            const HRESULT set_status = SetPersistent(module, policy);
            const auto wrong_policy =
                static_cast<LAVOpenJocOutputPolicy>(value == 6 ? 0 : value + 1);
            if (FAILED(set_status) || !ReportAndVerify(module, L"self-test", &policy) ||
                PolicyStateMatchesExpected(module, wrong_policy))
            {
                std::fwprintf(stderr, L"self-test policy failed: policy=%lu set_hr=0x%08lx\n",
                              static_cast<unsigned long>(value),
                              static_cast<unsigned long>(set_status));
                result = 1;
            }
        }
    }
    if (registry.Restore() != ERROR_SUCCESS || !registry.temporary_tree_absent())
        result = 1;
    const PolicyRegistrySnapshot real_after = SnapshotPolicyRegistry();
    if (!(real_before == real_after))
        result = 1;
    if (result == 0)
        std::wprintf(L"POLICY_CONTROL_SELF_TEST_COMPLETE policies=%zu "
                     L"registry_override_restored=1 temporary_tree_absent=1 "
                     L"real_policy_values_restored=1\n",
                     static_cast<std::size_t>(LAV_OPENJOC_OUTPUT_CONTRACT_COUNT));
    return result;
}
} // namespace

int wmain(const int argc, wchar_t **argv)
{
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status))
    {
        std::fwprintf(stderr, L"COM initialization failed: 0x%08lx\n",
                      static_cast<unsigned long>(com_status));
        return 1;
    }

    int result = 64;
    if (argc == 3 && wcscmp(argv[1], L"--self-test") == 0)
    {
        result = RunSelfTest(argv[2]);
    }
    else if ((argc == 3 && wcscmp(argv[1], L"--get") == 0) ||
             (argc == 4 && wcscmp(argv[1], L"--set-persistent") == 0))
    {
        const wchar_t *module_path = argv[argc - 1];
        PrivateComModule module(module_path);
        if (FAILED(module.status()))
        {
            std::fwprintf(stderr, L"private target activation failed: 0x%08lx\n",
                          static_cast<unsigned long>(module.status()));
            result = 1;
        }
        else if (argc == 3)
        {
            result = ReportAndVerify(module, L"get", nullptr) ? 0 : 1;
        }
        else
        {
            LAVOpenJocOutputPolicy policy = LAVOpenJocOutputPolicy::Stereo;
            if (!ParsePolicy(argv[2], &policy))
            {
                std::fwprintf(stderr, L"invalid policy: expected integer 0 through 7\n");
                result = 64;
            }
            else
            {
                const HRESULT set_status = SetPersistent(module, policy);
                result = SUCCEEDED(set_status) &&
                                 ReportAndVerify(module, L"set-persistent", &policy)
                             ? 0
                             : 1;
                if (FAILED(set_status))
                    std::fwprintf(stderr, L"persistent policy update failed: 0x%08lx\n",
                                  static_cast<unsigned long>(set_status));
            }
        }
    }
    else
    {
        std::fwprintf(stderr,
                      L"usage: OpenJocPolicyControl.exe --set-persistent <0..7> "
                      L"<absolute-target-LAVAudio.ax>\n"
                      L"   or: OpenJocPolicyControl.exe --get <absolute-target-LAVAudio.ax>\n"
                      L"   or: OpenJocPolicyControl.exe --self-test "
                      L"<absolute-target-LAVAudio.ax>\n");
    }
    CoUninitialize();
    return result;
}
