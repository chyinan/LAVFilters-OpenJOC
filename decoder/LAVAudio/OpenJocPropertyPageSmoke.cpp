/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Hidden-dialog integration smoke for shipped-layout and same-instance status pages.

// pattern: Imperative Shell

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

#include <commctrl.h>
#include <dshow.h>
#include <ocidl.h>

#include "ISpecifyPropertyPages2.h"
#include "LAVAudioSettings.h"
#include "LAVOpenJocDiagnostics.h"
#include "LAVOpenJocSettings.h"
#include "OpenJocBinauralSettings.h"

namespace
{
constexpr GUID kOpenJocLavAudio = {
    0x27247580, 0xc701, 0x40cd, {0x88, 0x6d, 0xe6, 0x18, 0xfc, 0x8c, 0x9f, 0xff}};
constexpr GUID kSettingsPage = {
    0x2d8f1801, 0xa70d, 0x48f4, {0xb7, 0x6b, 0x7f, 0x5a, 0xe0, 0x22, 0xab, 0x54}};
constexpr GUID kStatusPage = {
    0x20ed4a03, 0x6afd, 0x4fd9, {0x98, 0x0b, 0x2f, 0x61, 0x43, 0xaa, 0x08, 0x92}};
constexpr GUID kOpenJocPage = {
    0xb316b03c, 0x8c27, 0x4adb, {0xb4, 0x2b, 0x00, 0xde, 0xc7, 0x82, 0x25, 0xdf}};
constexpr GUID kAudioSettings = {
    0x4158a22b, 0x6553, 0x45d0, {0x80, 0x69, 0x24, 0x71, 0x6f, 0x8f, 0xf1, 0x71}};

constexpr int kOpenJocOutputPolicyControl = 1136;
constexpr int kOpenJocOutputGuidanceControl = 1144;
constexpr int kOpenJocOutputCompatControl = 1145;
constexpr int kOpenJocDialnormPolicyControl = 1140;
constexpr int kOpenJocVirtualLayoutControl = 1149;
constexpr int kOpenJocHrtfSourceControl = 1151;
constexpr int kOpenJocSofaFileControl = 1153;
constexpr int kOpenJocSofaBrowseControl = 1154;
constexpr int kOpenJocStatusPolicyControl = 1138;
constexpr int kOpenJocStatusAdmissionControl = 1139;
constexpr int kOpenJocStatusWarningControl = 1146;
constexpr int kOpenJocStatusReasonControl = 1147;
constexpr int kOpenJocStatusDetailsControl = 1148;
constexpr int kTrayIconControl = 1131;
constexpr int kOutputChannelControl = 1086;
constexpr int kOutputCodecControl = 1085;
constexpr int kOutputSampleRateControl = 1084;
constexpr int kOutputFormatControl = 1087;
constexpr int kVolumeControls[] = {1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047};
constexpr int kVolumeDescriptionControls[] = {1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055};

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

class PropertyPageSite final : public IPropertyPageSite
{
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IPropertyPageSite)
        {
            *object = static_cast<IPropertyPageSite *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&references_)); }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD flags) override
    {
        if ((flags & PROPPAGESTATUS_DIRTY) != 0)
            ++dirty_notifications_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleID(LCID *locale) override
    {
        if (!locale)
            return E_POINTER;
        *locale = GetThreadLocale();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPageContainer(IUnknown **container) override
    {
        if (!container)
            return E_POINTER;
        *container = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG *) override { return S_FALSE; }
    int dirty_notifications() const { return dirty_notifications_; }

  private:
    LONG references_ = 1;
    int dirty_notifications_ = 0;
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
    HRESULT CreateFilter(IBaseFilter **filter) const
    {
        if (!filter)
            return E_POINTER;
        *filter = nullptr;
        if (FAILED(status_) || !factory_)
            return FAILED(status_) ? status_ : E_FAIL;
        return factory_->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void **>(filter));
    }

  private:
    HMODULE module_ = nullptr;
    IClassFactory *factory_ = nullptr;
    HRESULT status_ = E_FAIL;
};

struct FindControlContext
{
    int id;
    HWND window;
};

BOOL CALLBACK FindControlCallback(HWND window, LPARAM parameter)
{
    auto *context = reinterpret_cast<FindControlContext *>(parameter);
    if (GetDlgCtrlID(window) == context->id)
    {
        context->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindControl(HWND parent, const int id)
{
    FindControlContext context{id, nullptr};
    EnumChildWindows(parent, FindControlCallback, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

std::wstring WindowText(HWND window)
{
    wchar_t text[512] = {};
    GetWindowTextW(window, text, static_cast<int>(std::size(text)));
    return text;
}

bool MeterLabelsMatch(HWND parent, const wchar_t *const (&expected)[8])
{
    for (std::size_t index = 0; index < std::size(kVolumeDescriptionControls); ++index)
    {
        const HWND label = FindControl(parent, kVolumeDescriptionControls[index]);
        if (!label || WindowText(label) != expected[index])
            return false;
    }
    return true;
}

bool MeterPositionsMatch(HWND parent, const int active_count)
{
    for (std::size_t index = 0; index < std::size(kVolumeControls); ++index)
    {
        const HWND meter = FindControl(parent, kVolumeControls[index]);
        const LRESULT expected = static_cast<int>(index) < active_count ? 38 : 0;
        if (!meter || SendMessageW(meter, PBM_GETPOS, 0, 0) != expected)
            return false;
    }
    return true;
}

class FakeStatusInstance final : public ILAVAudioStatus,
                                 public ILAVOpenJocSettings,
                                 public ILAVOpenJocStatus,
                                 public ILAVOpenJocDiagnostics2
{
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(ILAVAudioStatus))
            *object = static_cast<ILAVAudioStatus *>(this);
        else if (iid == __uuidof(ILAVOpenJocSettings))
            *object = static_cast<ILAVOpenJocSettings *>(this);
        else if (iid == __uuidof(ILAVOpenJocStatus))
            *object = static_cast<ILAVOpenJocStatus *>(this);
        else if (iid == __uuidof(ILAVOpenJocDiagnostics2))
            *object = static_cast<ILAVOpenJocDiagnostics2 *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&references_)); }

    BOOL STDMETHODCALLTYPE IsSampleFormatSupported(LAVAudioSampleFormat) override { return TRUE; }
    HRESULT STDMETHODCALLTYPE GetDecodeDetails(LPCSTR *codec, LPCSTR *format, int *channels,
                                               int *sample_rate, DWORD *mask) override
    {
        if (codec)
            *codec = "eac3";
        if (format)
            *format = "32-bit Floating-point";
        if (channels)
            *channels = output_channels_;
        if (sample_rate)
            *sample_rate = 48000;
        if (mask)
            *mask = output_mask_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOutputDetails(LPCSTR *format, int *channels, int *sample_rate,
                                               DWORD *mask) override
    {
        ++output_detail_queries_;
        if (output_result_ != S_OK)
            return output_result_;
        if (format)
            *format = "32-bit Floating-point";
        if (channels)
            *channels = output_channels_;
        if (sample_rate)
            *sample_rate = 48000;
        if (mask)
            *mask = output_mask_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnableVolumeStats() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DisableVolumeStats() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetChannelVolumeAverage(WORD channel, float *db) override
    {
        if (!db)
            return E_POINTER;
        ++volume_queries_;
        max_volume_channel_ = (std::max)(max_volume_channel_, static_cast<int>(channel));
        if (channel >= 8)
            return E_INVALIDARG;
        *db = -12.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetOutputPolicy(LAVOpenJocOutputPolicy *policy) override
    {
        ++policy_queries_;
        if (!policy)
            return E_POINTER;
        *policy = policy_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOutputPolicy(LAVOpenJocOutputPolicy policy) override
    {
        policy_ = policy;
        return S_OK;
    }
    BOOL STDMETHODCALLTYPE IsOpenJocAvailable() override { return TRUE; }
    LAVOpenJocAdmissionState STDMETHODCALLTYPE GetOpenJocAdmissionState() override
    {
        ++admission_queries_;
        return admission_;
    }

    HRESULT STDMETHODCALLTYPE GetOpenJocPlaybackDiagnostics(
        LAVOpenJocDiagnosticReason *reason, BOOL *warning, BOOL *failure_au_known,
        ULONGLONG *failure_au, LPWSTR detail, DWORD detail_capacity) override
    {
        if (!reason || !warning || !failure_au_known || !failure_au)
            return E_POINTER;
        if (detail_capacity > 0 && !detail)
            return E_POINTER;
        *reason = reason_;
        *warning = warning_;
        *failure_au_known = failure_au_known_;
        *failure_au = failure_au_;
        if (detail_capacity > 0)
            wcsncpy_s(detail, detail_capacity, detail_.c_str(), _TRUNCATE);
        return S_OK;
    }

    void SetOutput(const int channels, const DWORD mask, const LAVOpenJocOutputPolicy policy,
                   const LAVOpenJocAdmissionState admission)
    {
        output_channels_ = channels;
        output_mask_ = mask;
        policy_ = policy;
        admission_ = admission;
        output_result_ = S_OK;
        reason_ = LAVOpenJocDiagnosticNone;
        warning_ = FALSE;
        failure_au_known_ = FALSE;
        failure_au_ = 0;
        detail_.clear();
        volume_queries_ = 0;
        max_volume_channel_ = -1;
    }

    void SetDiagnostic(const LAVOpenJocDiagnosticReason reason, const BOOL warning,
                       const BOOL failure_au_known, const ULONGLONG failure_au,
                       const wchar_t *detail)
    {
        reason_ = reason;
        warning_ = warning;
        failure_au_known_ = failure_au_known;
        failure_au_ = failure_au;
        detail_ = detail ? detail : L"";
    }
    void SetOutputResult(const HRESULT result)
    {
        output_result_ = result;
        volume_queries_ = 0;
        max_volume_channel_ = -1;
    }
    int volume_queries() const { return volume_queries_; }
    int max_volume_channel() const { return max_volume_channel_; }
    int output_detail_queries() const { return output_detail_queries_; }
    int policy_queries() const { return policy_queries_; }
    int admission_queries() const { return admission_queries_; }

  private:
    LONG references_ = 1;
    int output_channels_ = 10;
    DWORD output_mask_ = 0x0002d60f;
    LAVOpenJocOutputPolicy policy_ = LAVOpenJocOutputPolicy::Layout514;
    LAVOpenJocAdmissionState admission_ = LAVOpenJocAdmissionOpenJoc;
    HRESULT output_result_ = S_OK;
    int volume_queries_ = 0;
    int max_volume_channel_ = -1;
    int output_detail_queries_ = 0;
    int policy_queries_ = 0;
    int admission_queries_ = 0;
    LAVOpenJocDiagnosticReason reason_ = LAVOpenJocDiagnosticNone;
    BOOL warning_ = FALSE;
    BOOL failure_au_known_ = FALSE;
    ULONGLONG failure_au_ = 0;
    std::wstring detail_;
};

HRESULT ActivatePage(IPropertyPage *page, IUnknown *object, PropertyPageSite *site, HWND parent, HWND *page_window)
{
    HRESULT hr = page->SetPageSite(site);
    if (SUCCEEDED(hr))
        hr = page->SetObjects(1, &object);
    RECT rect{0, 0, 640, 480};
    if (SUCCEEDED(hr))
        hr = page->Activate(parent, &rect, FALSE);
    if (SUCCEEDED(hr) && page_window)
        *page_window = GetWindow(parent, GW_CHILD);
    return hr;
}

void DisconnectPage(IPropertyPage *page, const bool active)
{
    if (!page)
        return;
    if (active)
        page->Deactivate();
    page->SetObjects(0, nullptr);
    page->SetPageSite(nullptr);
}

bool TestSettingsPageHasNoOpenJocControls(IBaseFilter *filter, ISpecifyPropertyPages2 *pages, HWND parent)
{
    IPropertyPage *page = nullptr;
    HRESULT hr = pages->CreatePage(kSettingsPage, &page);
    PropertyPageSite site;
    HWND page_window = nullptr;
    if (SUCCEEDED(hr))
        hr = ActivatePage(page, filter, &site, parent, &page_window);
    const bool active = SUCCEEDED(hr);
    if (SUCCEEDED(hr) &&
        (!page_window ||
         FindControl(page_window, kOpenJocOutputPolicyControl) != nullptr ||
         FindControl(page_window, kOpenJocDialnormPolicyControl) != nullptr ||
         FindControl(page_window, kTrayIconControl) == nullptr))
        hr = E_UNEXPECTED;

    DisconnectPage(page, active);
    Release(page);
    return SUCCEEDED(hr);
}

bool TestOpenJocPage(IBaseFilter *filter, ISpecifyPropertyPages2 *pages, HWND parent)
{
    constexpr struct
    {
        LAVOpenJocOutputPolicy policy;
        const wchar_t *label;
    } expected_policies[] = {
        {LAVOpenJocOutputPolicy::Stereo, L"Stereo (Speakers)"},
        {LAVOpenJocOutputPolicy::Binaural, L"Binaural (Headphones)"},
        {LAVOpenJocOutputPolicy::Layout51, L"5.1"},
        {LAVOpenJocOutputPolicy::Layout71, L"7.1"},
        {LAVOpenJocOutputPolicy::Layout512, L"5.1.2"},
        {LAVOpenJocOutputPolicy::Layout514, L"5.1.4"},
        {LAVOpenJocOutputPolicy::Layout712, L"7.1.2"},
        {LAVOpenJocOutputPolicy::Layout714, L"7.1.4"},
    };
    constexpr struct
    {
        LAVOpenJocDialnormPolicy policy;
        const wchar_t *label;
    } expected_dialnorm[] = {
        {LAVOpenJocDialnormPolicy::Calibrated, L"Calibrated (Recommended)"},
        {LAVOpenJocDialnormPolicy::UnityCompatibility, L"Unity / Compatibility"},
    };

    ILAVOpenJocSettings *settings = nullptr;
    ILAVOpenJocLevelSettings *level_settings = nullptr;
    ILAVOpenJocBinauralSettings *binaural_settings = nullptr;
    ITestRuntimeSettings *runtime = nullptr;
    HRESULT hr = filter->QueryInterface(__uuidof(ILAVOpenJocSettings), reinterpret_cast<void **>(&settings));
    if (SUCCEEDED(hr))
        hr = filter->QueryInterface(__uuidof(ILAVOpenJocLevelSettings),
                                    reinterpret_cast<void **>(&level_settings));
    if (SUCCEEDED(hr))
        hr = filter->QueryInterface(__uuidof(ILAVOpenJocBinauralSettings),
                                    reinterpret_cast<void **>(&binaural_settings));
    if (SUCCEEDED(hr))
        hr = filter->QueryInterface(kAudioSettings, reinterpret_cast<void **>(&runtime));
    if (SUCCEEDED(hr))
        hr = runtime->SetRuntimeConfig(TRUE);
    if (SUCCEEDED(hr))
        hr = settings->SetOutputPolicy(LAVOpenJocOutputPolicy::Layout714);
    if (SUCCEEDED(hr))
        hr = level_settings->SetDialnormPolicy(LAVOpenJocDialnormPolicy::Calibrated);

    IPropertyPage *page = nullptr;
    if (SUCCEEDED(hr))
        hr = pages->CreatePage(kOpenJocPage, &page);
    PropertyPageSite site;
    HWND page_window = nullptr;
    if (SUCCEEDED(hr))
        hr = ActivatePage(page, filter, &site, parent, &page_window);
    const bool active = SUCCEEDED(hr);

    HWND combo = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocOutputPolicyControl) : nullptr;
    HWND output_guidance = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocOutputGuidanceControl) : nullptr;
    HWND output_compat = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocOutputCompatControl) : nullptr;
    HWND dialnorm = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocDialnormPolicyControl) : nullptr;
    HWND virtual_layout = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocVirtualLayoutControl) : nullptr;
    HWND hrtf_source = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocHrtfSourceControl) : nullptr;
    HWND sofa_file = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocSofaFileControl) : nullptr;
    HWND sofa_browse = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocSofaBrowseControl) : nullptr;
    if (!combo || !output_guidance || !output_compat || !dialnorm || !virtual_layout || !hrtf_source ||
        !sofa_file || !sofa_browse ||
        WindowText(output_guidance) !=
            L"Speakers: choose the layout matching your playback system. Headphones: choose Binaural for HRTF spatial rendering." ||
        WindowText(output_compat) !=
            L"Stereo (Speakers) and Binaural (Headphones) both output 2-channel PCM with different rendering semantics." ||
        SendMessageW(combo, CB_GETCOUNT, 0, 0) != std::size(expected_policies) ||
        SendMessageW(combo, CB_GETCURSEL, 0, 0) != 7 ||
        SendMessageW(dialnorm, CB_GETCOUNT, 0, 0) != std::size(expected_dialnorm) ||
        SendMessageW(dialnorm, CB_GETCURSEL, 0, 0) != 0 ||
        SendMessageW(virtual_layout, CB_GETCOUNT, 0, 0) != 2 ||
        SendMessageW(virtual_layout, CB_GETCURSEL, 0, 0) != 0 ||
        SendMessageW(hrtf_source, CB_GETCOUNT, 0, 0) != 2 ||
        SendMessageW(hrtf_source, CB_GETCURSEL, 0, 0) != 0 ||
        WindowText(sofa_file) != L"" || IsWindowEnabled(virtual_layout) || IsWindowEnabled(hrtf_source) ||
        IsWindowEnabled(sofa_file) || IsWindowEnabled(sofa_browse))
        hr = E_UNEXPECTED;
    for (std::size_t index = 0; SUCCEEDED(hr) && index < std::size(expected_policies); ++index)
    {
        wchar_t combo_label[32] = {};
        SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(combo_label));
        if (SendMessageW(combo, CB_GETITEMDATA, index, 0) !=
                static_cast<LRESULT>(expected_policies[index].policy) ||
            std::wstring(combo_label) != expected_policies[index].label)
            hr = E_UNEXPECTED;
    }
    for (std::size_t index = 0; SUCCEEDED(hr) && index < std::size(expected_dialnorm); ++index)
    {
        wchar_t combo_label[64] = {};
        SendMessageW(dialnorm, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(combo_label));
        if (SendMessageW(dialnorm, CB_GETITEMDATA, index, 0) !=
                static_cast<LRESULT>(expected_dialnorm[index].policy) ||
            std::wstring(combo_label) != expected_dialnorm[index].label)
            hr = E_UNEXPECTED;
    }
    constexpr const wchar_t *expected_virtual_layouts[] = {
        L"7.1.4 (Recommended)", L"9.1.6 (Experimental)"};
    for (std::size_t index = 0; SUCCEEDED(hr) && index < std::size(expected_virtual_layouts); ++index)
    {
        wchar_t label[64] = {};
        SendMessageW(virtual_layout, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label));
        if (std::wstring(label) != expected_virtual_layouts[index] ||
            SendMessageW(virtual_layout, CB_GETITEMDATA, index, 0) != static_cast<LRESULT>(index))
            hr = E_UNEXPECTED;
    }
    constexpr const wchar_t *expected_hrtf_sources[] = {
        L"Built-in SADIE II D1 (Recommended)", L"Custom SOFA..."};
    for (std::size_t index = 0; SUCCEEDED(hr) && index < std::size(expected_hrtf_sources); ++index)
    {
        wchar_t label[96] = {};
        SendMessageW(hrtf_source, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label));
        if (std::wstring(label) != expected_hrtf_sources[index] ||
            SendMessageW(hrtf_source, CB_GETITEMDATA, index, 0) != static_cast<LRESULT>(index))
            hr = E_UNEXPECTED;
    }

    if (SUCCEEDED(hr) &&
        (SendMessageW(combo, CB_SETCURSEL, 1, 0) != 1 ||
         SendMessageW(hrtf_source, CB_SETCURSEL, 1, 0) != 1))
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
    {
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocOutputPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
        if (!IsWindowEnabled(virtual_layout) || !IsWindowEnabled(hrtf_source) ||
            IsWindowEnabled(sofa_file) || IsWindowEnabled(sofa_browse))
            hr = E_UNEXPECTED;
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocHrtfSourceControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(hrtf_source));
        if (!IsWindowEnabled(sofa_file) || !IsWindowEnabled(sofa_browse))
            hr = E_UNEXPECTED;
        if (SUCCEEDED(hr) && SendMessageW(virtual_layout, CB_SETCURSEL, 1, 0) != 1)
            hr = E_UNEXPECTED;
        if (SUCCEEDED(hr))
            SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocVirtualLayoutControl, CBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(virtual_layout));
        if (SUCCEEDED(hr) &&
            SendMessageW(virtual_layout, CB_GETITEMDATA, 1, 0) !=
                static_cast<LRESULT>(LAVOpenJocBinauralVirtualLayout::Layout916))
            hr = E_UNEXPECTED;
        SendMessageW(virtual_layout, CB_SETCURSEL, 0, 0);
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocVirtualLayoutControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(virtual_layout));
        SendMessageW(hrtf_source, CB_SETCURSEL, 0, 0);
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocHrtfSourceControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(hrtf_source));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocOutputPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
    }

    if (SUCCEEDED(hr) &&
        (SendMessageW(combo, CB_SETCURSEL, 0, 0) != 0 ||
         SendMessageW(dialnorm, CB_SETCURSEL, 1, 0) != 1))
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
    {
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocOutputPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocDialnormPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(dialnorm));
        if (site.dirty_notifications() < 2)
            hr = E_UNEXPECTED;
    }

    DisconnectPage(page, active);
    Release(page);

    LAVOpenJocOutputPolicy policy = LAVOpenJocOutputPolicy::Stereo;
    LAVOpenJocDialnormPolicy dialnorm_policy = LAVOpenJocDialnormPolicy::UnityCompatibility;
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&policy);
    if (SUCCEEDED(hr))
        hr = level_settings->GetDialnormPolicy(&dialnorm_policy);
    if (SUCCEEDED(hr) && (policy != LAVOpenJocOutputPolicy::Layout714 ||
                          dialnorm_policy != LAVOpenJocDialnormPolicy::Calibrated))
        hr = E_UNEXPECTED;

    page = nullptr;
    page_window = nullptr;
    if (SUCCEEDED(hr))
        hr = pages->CreatePage(kOpenJocPage, &page);
    PropertyPageSite apply_site;
    if (SUCCEEDED(hr))
        hr = ActivatePage(page, filter, &apply_site, parent, &page_window);
    const bool apply_active = SUCCEEDED(hr);
    combo = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocOutputPolicyControl) : nullptr;
    dialnorm = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocDialnormPolicyControl) : nullptr;
    virtual_layout = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocVirtualLayoutControl) : nullptr;
    hrtf_source = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocHrtfSourceControl) : nullptr;
    sofa_file = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocSofaFileControl) : nullptr;
    sofa_browse = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocSofaBrowseControl) : nullptr;
    if (!combo || !dialnorm || !virtual_layout || !hrtf_source || !sofa_file || !sofa_browse ||
        SendMessageW(combo, CB_GETCURSEL, 0, 0) != 7 ||
        SendMessageW(dialnorm, CB_GETCURSEL, 0, 0) != 0)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr) &&
        (SendMessageW(combo, CB_SETCURSEL, 0, 0) != 0 ||
         SendMessageW(dialnorm, CB_SETCURSEL, 1, 0) != 1))
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
    {
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocOutputPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocDialnormPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(dialnorm));
        hr = page->Apply();
    }
    if (SUCCEEDED(hr))
        hr = settings->GetOutputPolicy(&policy);
    if (SUCCEEDED(hr))
        hr = level_settings->GetDialnormPolicy(&dialnorm_policy);
    if (SUCCEEDED(hr) && (policy != LAVOpenJocOutputPolicy::Stereo ||
                          dialnorm_policy != LAVOpenJocDialnormPolicy::UnityCompatibility))
        hr = E_UNEXPECTED;

    DisconnectPage(page, apply_active);
    Release(page);
    page = nullptr;
    page_window = nullptr;
    if (SUCCEEDED(hr))
        hr = pages->CreatePage(kOpenJocPage, &page);
    PropertyPageSite reopen_site;
    if (SUCCEEDED(hr))
        hr = ActivatePage(page, filter, &reopen_site, parent, &page_window);
    const bool reopen_active = SUCCEEDED(hr);
    combo = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocOutputPolicyControl) : nullptr;
    dialnorm = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocDialnormPolicyControl) : nullptr;
    virtual_layout = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocVirtualLayoutControl) : nullptr;
    hrtf_source = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocHrtfSourceControl) : nullptr;
    sofa_file = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocSofaFileControl) : nullptr;
    sofa_browse = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocSofaBrowseControl) : nullptr;
    if (!combo || !dialnorm || !virtual_layout || !hrtf_source || !sofa_file || !sofa_browse ||
        SendMessageW(combo, CB_GETCURSEL, 0, 0) != 0 ||
        SendMessageW(dialnorm, CB_GETCURSEL, 0, 0) != 1)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr) &&
        (SendMessageW(virtual_layout, CB_GETCURSEL, 0, 0) != 0 ||
         SendMessageW(hrtf_source, CB_GETCURSEL, 0, 0) != 0 || IsWindowEnabled(virtual_layout) ||
         IsWindowEnabled(hrtf_source) || IsWindowEnabled(sofa_file) || IsWindowEnabled(sofa_browse)))
        hr = E_UNEXPECTED;

    if (SUCCEEDED(hr) && SendMessageW(dialnorm, CB_SETCURSEL, 0, 0) != 0)
        hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
    {
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(kOpenJocDialnormPolicyControl, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(dialnorm));
        hr = page->Apply();
    }
    if (SUCCEEDED(hr))
        hr = level_settings->GetDialnormPolicy(&dialnorm_policy);
    const bool passed = SUCCEEDED(hr) && policy == LAVOpenJocOutputPolicy::Stereo &&
                        dialnorm_policy == LAVOpenJocDialnormPolicy::Calibrated;

    DisconnectPage(page, reopen_active);
    Release(page);
    Release(runtime);
    Release(binaural_settings);
    Release(level_settings);
    Release(settings);
    return passed;
}

bool TestStatusPage(ISpecifyPropertyPages2 *pages, HWND parent)
{
    const wchar_t *const labels_514[8] = {L"L", L"R", L"C", L"LFE", L"SL", L"SR", L"TFL", L"TFR"};
    const wchar_t *const labels_714[8] = {L"L", L"R", L"C", L"LFE", L"BL", L"BR", L"SL", L"SR"};
    const wchar_t *const labels_stereo[8] = {L"L", L"R", L"", L"", L"", L"", L"", L""};
    const wchar_t *const labels_empty[8] = {L"", L"", L"", L"", L"", L"", L"", L""};

    IPropertyPage *page = nullptr;
    HRESULT hr = pages->CreatePage(kStatusPage, &page);
    PropertyPageSite site;
    FakeStatusInstance status;
    HWND page_window = nullptr;
    IUnknown *status_object = static_cast<ILAVAudioStatus *>(&status);
    if (SUCCEEDED(hr))
        hr = ActivatePage(page, status_object, &site, parent, &page_window);
    const bool active = SUCCEEDED(hr);

    HWND output = SUCCEEDED(hr) ? FindControl(page_window, kOutputChannelControl) : nullptr;
    HWND codec = SUCCEEDED(hr) ? FindControl(page_window, kOutputCodecControl) : nullptr;
    HWND sample_rate = SUCCEEDED(hr) ? FindControl(page_window, kOutputSampleRateControl) : nullptr;
    HWND format = SUCCEEDED(hr) ? FindControl(page_window, kOutputFormatControl) : nullptr;
    HWND policy = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocStatusPolicyControl) : nullptr;
    HWND admission = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocStatusAdmissionControl) : nullptr;
    HWND warning = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocStatusWarningControl) : nullptr;
    HWND reason = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocStatusReasonControl) : nullptr;
    HWND details = SUCCEEDED(hr) ? FindControl(page_window, kOpenJocStatusDetailsControl) : nullptr;
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (!output || !codec || !sample_rate || !format || !policy || !admission ||
        !warning || !reason || !details ||
        WindowText(output) != L"10 / 0x2d60f" || WindowText(codec) != L"PCM" ||
        WindowText(sample_rate) != L"48000" || WindowText(format) != L"32-bit Floating-point" ||
        WindowText(policy) != L"5.1.4" || WindowText(admission) != L"OpenJOC" ||
        !WindowText(warning).empty() || !WindowText(reason).empty() ||
        !WindowText(details).empty() ||
        !MeterLabelsMatch(page_window, labels_514))
        hr = E_UNEXPECTED;

    status.SetOutput(10, 0x0002d60f, LAVOpenJocOutputPolicy::Layout514, LAVOpenJocAdmissionOpenJoc);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) && (status.volume_queries() != 8 || status.max_volume_channel() != 7 ||
                          !MeterPositionsMatch(page_window, 8)))
        hr = E_UNEXPECTED;

    status.SetOutput(12, 0x0002d63f, LAVOpenJocOutputPolicy::Layout714, LAVOpenJocAdmissionStockEac3);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) && (WindowText(output) != L"12 / 0x2d63f" || WindowText(codec) != L"PCM" ||
                          WindowText(sample_rate) != L"48000" ||
                          WindowText(format) != L"32-bit Floating-point" || WindowText(policy) != L"7.1.4" ||
                          WindowText(admission) != L"Stock decoder" || status.volume_queries() != 8 ||
                          status.max_volume_channel() != 7 || status.output_detail_queries() < 3 ||
                          status.policy_queries() < 3 || status.admission_queries() < 3 ||
                          !MeterLabelsMatch(page_window, labels_714) || !MeterPositionsMatch(page_window, 8)))
        hr = E_UNEXPECTED;

    status.SetOutput(2, 0x00000003, LAVOpenJocOutputPolicy::Stereo, LAVOpenJocAdmissionOpenJoc);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) &&
        (status.volume_queries() != 2 || status.max_volume_channel() != 1 ||
         !MeterLabelsMatch(page_window, labels_stereo) || !MeterPositionsMatch(page_window, 2)))
        hr = E_UNEXPECTED;

    status.SetOutput(12, 0x0002d63f, LAVOpenJocOutputPolicy::Layout714,
                     LAVOpenJocAdmissionStockOpenJocFallback);
    status.SetDiagnostic(LAVOpenJocDiagnosticMalformedJocMetadata, TRUE, TRUE, 0,
                         L"EMDF declares 573 bytes, but only 507 are available.");
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) &&
        (WindowText(admission) != L"Stock decoder (OpenJOC fallback)" ||
         WindowText(warning) != L"Warning" ||
         WindowText(reason) != L"Malformed JOC metadata" ||
         WindowText(details) != L"AU 0: EMDF declares 573 bytes, but only 507 are available."))
        hr = E_UNEXPECTED;

    status.SetOutput(2, 0x00000003, LAVOpenJocOutputPolicy::Binaural, LAVOpenJocAdmissionOpenJoc);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) &&
        (WindowText(policy) != L"Binaural (Headphones)" ||
         WindowText(admission) != L"OpenJOC" ||
         !WindowText(warning).empty() || !WindowText(reason).empty() ||
         !WindowText(details).empty() || !MeterLabelsMatch(page_window, labels_stereo)))
        hr = E_UNEXPECTED;

    status.SetDiagnostic(LAVOpenJocDiagnosticBinauralHrtfConfiguration, TRUE, FALSE, 0,
                         L"Binaural HRTF configuration error: Custom SOFA could not be opened: missing.sofa");
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) &&
        (WindowText(warning) != L"Warning" || WindowText(reason) != L"Binaural HRTF configuration" ||
         WindowText(details) !=
             L"Binaural HRTF configuration error: Custom SOFA could not be opened: missing.sofa"))
        hr = E_UNEXPECTED;

    status.SetOutputResult(S_FALSE);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) &&
        (WindowText(codec) != L"Bitstreaming" || status.volume_queries() != 0 ||
         !MeterLabelsMatch(page_window, labels_empty) || !MeterPositionsMatch(page_window, 0)))
        hr = E_UNEXPECTED;

    status.SetOutput(12, 0x0002d63f, LAVOpenJocOutputPolicy::Layout714, LAVOpenJocAdmissionStockEac3);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    status.SetOutputResult(E_UNEXPECTED);
    if (SUCCEEDED(hr))
        SendMessageW(page_window, WM_TIMER, 1, 0);
    if (SUCCEEDED(hr) &&
        (!WindowText(output).empty() || !WindowText(codec).empty() || !WindowText(sample_rate).empty() ||
         !WindowText(format).empty() || status.volume_queries() != 0 ||
         !MeterLabelsMatch(page_window, labels_empty) || !MeterPositionsMatch(page_window, 0)))
        hr = E_UNEXPECTED;

    DisconnectPage(page, active);
    Release(page);
    return SUCCEEDED(hr);
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
    {
        std::fwprintf(stderr, L"usage: OpenJocPropertyPageSmoke.exe <OpenJOC LAVAudio.ax>\n");
        return 2;
    }

    InitCommonControls();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    HWND parent = CreateWindowExW(0, L"STATIC", L"OpenJOC property-page smoke", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!parent)
    {
        std::fwprintf(stderr, L"CreateWindowEx failed: %lu\n", GetLastError());
        return 1;
    }

    bool passed = false;
    {
        FilterModule module(argv[1]);
        IBaseFilter *filter = nullptr;
        ISpecifyPropertyPages2 *pages = nullptr;
        HRESULT hr = module.CreateFilter(&filter);
        if (SUCCEEDED(hr))
            hr = filter->QueryInterface(__uuidof(ISpecifyPropertyPages2), reinterpret_cast<void **>(&pages));
        const bool settings_page = SUCCEEDED(hr) && TestSettingsPageHasNoOpenJocControls(filter, pages, parent);
        const bool openjoc_page = settings_page && TestOpenJocPage(filter, pages, parent);
        const bool status_page = openjoc_page && TestStatusPage(pages, parent);
        passed = settings_page && openjoc_page && status_page;
        Release(pages);
        Release(filter);
    }
    DestroyWindow(parent);
    if (!passed)
    {
        std::fwprintf(stderr, L"OpenJOC property-page/status smoke failed\n");
        return 1;
    }
    std::wprintf(L"OpenJOC property-page/status smoke passed\n");
    return 0;
}
