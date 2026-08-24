/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Side-by-side COM identity smoke test.

#include <windows.h>

#include <cstdio>
#include <string>

#include <dshow.h>

using DllGetClassObjectProc = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, LPVOID *);

template <typename T> void Release(T *&value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

int wmain(int argc, wchar_t **argv)
{
    static const GUID kOpenJocLavAudio = {
        0x27247580, 0xc701, 0x40cd, {0x88, 0x6d, 0xe6, 0x18, 0xfc, 0x8c, 0x9f, 0xff}};
    static const GUID kStockLavAudio = {
        0xe8e73b6b, 0x4cb3, 0x44a4, {0xbe, 0x99, 0x4f, 0x7b, 0xcb, 0x96, 0xe4, 0x91}};
    static const GUID kAudioSettings = {
        0x4158a22b, 0x6553, 0x45d0, {0x80, 0x69, 0x24, 0x71, 0x6f, 0x8f, 0xf1, 0x71}};
    static const GUID kOpenJocSettings = {
        0x6b97fd1c, 0xb463, 0x4b5e, {0x93, 0x49, 0xcd, 0x8b, 0x96, 0x4d, 0x6b, 0x46}};

    if (argc < 2 || argc > 3)
    {
        std::fwprintf(stderr, L"usage: LAVAudioIdentitySmoke.exe <LAVAudio.ax> [stock]\n");
        return 2;
    }
    const bool expect_stock = argc == 3 && _wcsicmp(argv[2], L"stock") == 0;
    const GUID &expected_class_id = expect_stock ? kStockLavAudio : kOpenJocLavAudio;

    std::wstring directory(argv[1]);
    const std::size_t separator = directory.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
        SetDllDirectoryW(directory.substr(0, separator).c_str());

    HMODULE module = LoadLibraryW(argv[1]);
    if (!module)
    {
        std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    auto get_class_object = reinterpret_cast<DllGetClassObjectProc>(GetProcAddress(module, "DllGetClassObject"));
    IClassFactory *factory = nullptr;
    IBaseFilter *filter = nullptr;
    HRESULT hr = get_class_object ? get_class_object(expected_class_id, IID_IClassFactory,
                                                      reinterpret_cast<void **>(&factory))
                                  : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (SUCCEEDED(hr))
        hr = factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void **>(&filter));
    if (SUCCEEDED(hr))
    {
        IPersist *persist = nullptr;
        CLSID class_id{};
        hr = filter->QueryInterface(IID_IPersist, reinterpret_cast<void **>(&persist));
        if (SUCCEEDED(hr))
            hr = persist->GetClassID(&class_id);
        if (SUCCEEDED(hr) && class_id != expected_class_id)
            hr = E_UNEXPECTED;
        Release(persist);
    }
    if (SUCCEEDED(hr))
    {
        IUnknown *audio_settings = nullptr;
        hr = filter->QueryInterface(kAudioSettings, reinterpret_cast<void **>(&audio_settings));
        Release(audio_settings);
    }
    if (SUCCEEDED(hr))
    {
        IUnknown *openjoc_settings = nullptr;
        const HRESULT settings_hr =
            filter->QueryInterface(kOpenJocSettings, reinterpret_cast<void **>(&openjoc_settings));
        if ((!expect_stock && settings_hr != S_OK) || (expect_stock && settings_hr != E_NOINTERFACE))
            hr = E_UNEXPECTED;
        Release(openjoc_settings);
    }

    if (FAILED(hr))
        std::fwprintf(stderr, L"identity failed: 0x%08lx\n", static_cast<unsigned long>(hr));
    else
        std::wprintf(expect_stock ? L"stock identity passed\n" : L"side-by-side identity passed\n");

    Release(filter);
    Release(factory);
    FreeLibrary(module);
    return FAILED(hr) ? 1 : 0;
}
