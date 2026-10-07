/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
// Portable checks of the native harness's unmodified type and gain oracles.
// The adapter is not COM, an allocator, a splitter, or a decoder.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
struct GUID {
    std::uint32_t Data1;
    std::uint16_t Data2, Data3;
    std::uint8_t Data4[8];
    bool operator==(const GUID &r) const { return std::memcmp(this, &r, sizeof(*this)) == 0; }
    bool operator!=(const GUID &r) const { return !(*this == r); }
};
constexpr GUID MEDIATYPE_Audio{1, 0, 0, {}}, MEDIASUBTYPE_PCM{2, 0, 0, {}},
    MEDIASUBTYPE_IEEE_FLOAT{3, 0, 0, {}}, FORMAT_WaveFormatEx{4, 0, 0, {}};
constexpr WORD WAVE_FORMAT_PCM = 1, WAVE_FORMAT_IEEE_FLOAT = 3, WAVE_FORMAT_EXTENSIBLE = 0xfffe;
constexpr bool FALSE = false;
#pragma pack(push, 1)
struct WAVEFORMATEX {
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct WAVEFORMATEXTENSIBLE {
    WAVEFORMATEX Format;
    union { WORD wValidBitsPerSample; } Samples;
    DWORD dwChannelMask;
    GUID SubFormat;
};
#pragma pack(pop)
static_assert(sizeof(WAVEFORMATEX) == 18 && sizeof(WAVEFORMATEXTENSIBLE) == 40, "Windows wave ABI");
struct AM_MEDIA_TYPE {
    GUID majortype{}, subtype{}, formattype{};
    DWORD cbFormat = 0;
    BYTE *pbFormat = nullptr;
};
struct CMediaType : AM_MEDIA_TYPE {
    std::vector<BYTE> storage;
    DWORD sample_size = 0;
    CMediaType() = default;
    explicit CMediaType(const AM_MEDIA_TYPE &r) : AM_MEDIA_TYPE(r) {
        if (r.pbFormat) storage.assign(r.pbFormat, r.pbFormat + r.cbFormat);
        pbFormat = storage.empty() ? nullptr : storage.data();
    }
    CMediaType &operator=(const CMediaType &r) {
        if (this != &r) {
            static_cast<AM_MEDIA_TYPE &>(*this) = r;
            storage = r.storage; sample_size = r.sample_size;
            pbFormat = storage.empty() ? nullptr : storage.data();
        }
        return *this;
    }
    CMediaType(const CMediaType &r) : AM_MEDIA_TYPE(r), storage(r.storage), sample_size(r.sample_size)
    { pbFormat = storage.data(); }
    void SetType(const GUID *v) { majortype = *v; }
    void SetSubtype(const GUID *v) { subtype = *v; }
    void SetFormatType(const GUID *v) { formattype = *v; }
    void SetSampleSize(DWORD v) { sample_size = v; }
    void SetTemporalCompression(bool) {}
    bool SetFormat(BYTE *p, DWORD n) { storage.assign(p, p + n); pbFormat = storage.data(); cbFormat = n; return true; }
    void InitMediaType() { cbFormat = 0; pbFormat = nullptr; }
};
#include "PcmTrackHarnessMethods.inc"
std::vector<BYTE> bytes(const std::vector<float> &samples) {
    std::vector<BYTE> result(samples.size() * sizeof(float));
    std::memcpy(result.data(), samples.data(), result.size());
    return result;
}
// Exercise the actual native Receive method. The base adapter deliberately
// validates without changing m_mt, matching DirectShow CBaseInputPin behavior.
using HRESULT = int;
#define STDMETHODIMP HRESULT
#define SUCCEEDED(value) ((value) >= 0)
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_UNEXPECTED = -1, E_FAIL = -2, VFW_E_TIMEOUT = -3;
namespace openjoc_harness_core {
bool ExactMediaTypeEqual(const AM_MEDIA_TYPE &a, const AM_MEDIA_TYPE &b) {
    return a.majortype == b.majortype && a.subtype == b.subtype && a.formattype == b.formattype &&
        a.cbFormat == b.cbFormat && (!a.cbFormat || (a.pbFormat && b.pbFormat &&
            std::memcmp(a.pbFormat, b.pbFormat, a.cbFormat) == 0));
}
}
struct IMediaSample {
    CMediaType type;
    HRESULT type_status = S_OK;
    HRESULT GetMediaType(AM_MEDIA_TYPE **out) {
        *out = type_status == S_OK ? new AM_MEDIA_TYPE(type) : nullptr;
        return type_status;
    }
};
void DeleteMediaType(AM_MEDIA_TYPE *type) { delete type; }
struct StrictCaptureSink {
    HRESULT capture_status = S_OK;
    int captured = 0;
    bool running = true;
    bool WaitUntilRunning() const { return running; }
    HRESULT RecordSample(IMediaSample *) { ++captured; return capture_status; }
};
struct CBaseInputPin {
    CMediaType m_mt;
    HRESULT base_status = S_OK;
    HRESULT Receive(IMediaSample *) { return base_status; }
};
struct StrictCaptureInputPin : CBaseInputPin {
    StrictCaptureSink *owner_ = nullptr;
    HRESULT commit_status = S_OK;
    int commits = 0;
    HRESULT SetMediaType(const CMediaType *type) {
        ++commits;
        if (commit_status == S_OK) m_mt = *type;
        return commit_status;
    }
    HRESULT Receive(IMediaSample *sample);
};
#include "PcmTrackReceiveMethod.inc"
void test_receiver_commits_only_successful_delivered_types() {
    StrictCaptureSink sink;
    StrictCaptureInputPin pin;
    pin.owner_ = &sink;
    const auto old_type = BuildTrackPcmType(2, 16, 3, false);
    const auto new_type = BuildTrackPcmType(12, 32, 0x2d63f, true);
    pin.m_mt = old_type;
    IMediaSample sample;
    sample.type = new_type;
    // Primary red/green check: the old sink captured the new type but retained
    // its previous ConnectionMediaType, despite reporting successful Receive.
    assert(pin.Receive(&sample) == S_OK);
    assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, new_type));
    pin.m_mt = old_type;
    pin.commits = 0;
    sink.captured = 0;
    for (const HRESULT rejected : {S_FALSE, E_FAIL}) {
        pin.base_status = rejected;
        assert(pin.Receive(&sample) == rejected);
        assert(sink.captured == 0 && pin.commits == 0);
        assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, old_type));
    }
    pin.base_status = S_OK;
    for (const HRESULT rejected : {S_FALSE, E_FAIL}) {
        sink.capture_status = rejected;
        assert(pin.Receive(&sample) == rejected);
        assert(pin.commits == 0);
        assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, old_type));
    }
    sink.capture_status = S_OK;
    pin.commit_status = E_FAIL;
    assert(pin.Receive(&sample) == E_FAIL && pin.commits == 1);
    assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, old_type));
    pin.commit_status = S_OK;
    assert(pin.Receive(&sample) == S_OK && pin.commits == 2);
    assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, new_type));
    assert(pin.Receive(&sample) == S_OK && pin.commits == 2); // same-type no-op
    sample.type_status = S_FALSE;
    assert(pin.Receive(&sample) == S_OK && pin.commits == 2); // no-type no-op
    sample.type_status = E_FAIL;
    assert(pin.Receive(&sample) == E_UNEXPECTED && pin.commits == 2);
    assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, new_type));
    sample.type_status = S_OK;
    sample.type = old_type;
    assert(pin.Receive(&sample) == S_OK && pin.commits == 3);
    assert(openjoc_harness_core::ExactMediaTypeEqual(pin.m_mt, old_type));
    sink.running = false;
    assert(pin.Receive(&sample) == VFW_E_TIMEOUT && pin.commits == 3);
    std::cout << "PCM_TRACK_RECEIVER_COMMIT_PASS (extracted native Receive)\n";
}
int main() {
    test_receiver_commits_only_successful_delivered_types();
    const PcmTrackCase cases[] = {
        {L"s16", L"", L"", 2, 16, 3, false, false, false},
        {L"f32", L"", L"", 2, 32, 3, true, false, false},
        {L"f64", L"", L"", 2, 64, 3, true, false, false},
        {L"s24", L"", L"", 6, 24, 0x3f, false, false, false},
        {L"s24-96k", L"", L"", 6, 24, 0x3f, false, false, false, 96000},
        {L"f32-96k", L"", L"", 2, 32, 3, true, false, false, 96000},
    };
    for (const auto &test : cases) {
        auto type = BuildTrackPcmType(test.channels, test.bits, test.mask, test.floating, 0, test.sample_rate);
        assert(TrackInputMetadataMatches(type, test));
        assert(type.sample_size == test.channels * test.bits / 8);
        auto *wave = reinterpret_cast<WAVEFORMATEX *>(type.pbFormat);
        assert(wave->nSamplesPerSec == test.sample_rate);
        assert(wave->nAvgBytesPerSec == wave->nBlockAlign * test.sample_rate);
        wave->nSamplesPerSec = test.sample_rate == 96000 ? 48000 : 96000;
        assert(!TrackInputMetadataMatches(type, test));
        wave->nSamplesPerSec = test.sample_rate;
        const WORD saved_bits = reinterpret_cast<WAVEFORMATEX *>(type.pbFormat)->wBitsPerSample;
        reinterpret_cast<WAVEFORMATEX *>(type.pbFormat)->wBitsPerSample = 8;
        assert(!TrackInputMetadataMatches(type, test));
        reinterpret_cast<WAVEFORMATEX *>(type.pbFormat)->wBitsPerSample = saved_bits;
        type.cbFormat = 17;
        assert(!TrackInputMetadataMatches(type, test));
    }
    auto padded = BuildTrackPcmType(6, 32, 0x3f, false, 24);
    const auto &ext = *reinterpret_cast<WAVEFORMATEXTENSIBLE *>(padded.pbFormat);
    assert(ext.Format.wBitsPerSample == 32 && ext.Samples.wValidBitsPerSample == 24);
    assert(ext.Format.nBlockAlign == 24 && ext.Format.nAvgBytesPerSec == 1152000);
    auto packed = BuildTrackPcmType(6, 24, 0x3f, false);
    assert(reinterpret_cast<WAVEFORMATEX *>(packed.pbFormat)->nBlockAlign == 18);
    auto f64_output = BuildTrackPcmType(2, 32, 3, true);
    assert(f64_output.cbFormat == 18 && f64_output.sample_size == 8);
    // LAV's existing CreateMediaType uses extensible above 48 kHz, including
    // stereo FP32. Reject a basic-header expectation instead of accepting a
    // decoder fallback or normalizing away the exact representation.
    for (bool floating : {false, true})
    {
        const WORD bits = floating ? 32 : 16;
        auto high_rate = BuildTrackPcmType(2, bits, 3, floating, 0, 96000);
        assert(high_rate.cbFormat == sizeof(WAVEFORMATEXTENSIBLE));
        const auto &wave = *reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(high_rate.pbFormat);
        assert(wave.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.Format.cbSize == 22);
        assert(wave.Format.nChannels == 2 && wave.Format.nSamplesPerSec == 96000);
        assert(wave.Format.wBitsPerSample == bits && wave.Samples.wValidBitsPerSample == bits);
        assert(wave.Format.nBlockAlign == 2 * bits / 8);
        assert(wave.Format.nAvgBytesPerSec == 96000 * wave.Format.nBlockAlign);
        assert(wave.dwChannelMask == 3);
        assert(wave.SubFormat == (floating ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM));
        assert(high_rate.subtype == wave.SubFormat && high_rate.sample_size == wave.Format.nBlockAlign);
        auto normal_rate = BuildTrackPcmType(2, bits, 3, floating);
        assert(normal_rate.cbFormat == sizeof(WAVEFORMATEX));
        assert(reinterpret_cast<const WAVEFORMATEX *>(normal_rate.pbFormat)->wFormatTag ==
               (floating ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM));
    }
    auto malformed = BuildTrackPcmType(2, 16, 3, false);
    reinterpret_cast<WAVEFORMATEX *>(malformed.pbFormat)->nAvgBytesPerSec++;
    assert(!TrackInputMetadataMatches(malformed, cases[0]));
    auto flac = BuildTrackPcmType(2, 16, 3, false);
    flac.subtype = {0xf1ac, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    flac.storage.resize(52); flac.pbFormat = flac.storage.data(); flac.cbFormat = 52;
    reinterpret_cast<WAVEFORMATEX *>(flac.pbFormat)->cbSize = 34;
    const PcmTrackCase control{L"flac", L"", L"", 2, 16, 3, false, true, false};
    assert(TrackInputMetadataMatches(flac, control));
    --flac.cbFormat;
    assert(!TrackInputMetadataMatches(flac, control));
    assert(TrackSampleDurationMatches(0, 853333, 8192 * 18, 18, 96000));
    assert(TrackSampleDurationMatches(0, 853334, 8192 * 18, 18, 96000));
    assert(!TrackSampleDurationMatches(0, 1706666, 8192 * 18, 18, 96000));
    assert(TrackSampleDurationMatches(0, 1706666, 8192 * 18, 18, 48000));
    assert(!TrackSampleDurationMatches(0, 853333, 8192 * 18, 18, 48000));
    assert(!TrackSampleDurationMatches(-1, 853333, 8192 * 18, 18, 96000));
    assert(!TrackSampleDurationMatches(0, 853333, 8192 * 18 - 1, 18, 96000));
    const std::vector<float> unity{-0.5f, 0, 0.125f, -0.25f, 0.75f, 0.33f};
    auto boosted = unity;
    for (auto &sample : boosted) sample *= static_cast<float>(std::pow(10.0, 6.0 / 20.0));
    assert(VerifyTrackGain(bytes(unity), bytes(boosted)));
    assert(!VerifyTrackGain(bytes(unity), bytes(unity)));
    std::swap(boosted[0], boosted[1]);
    assert(!VerifyTrackGain(bytes(unity), bytes(boosted)));
    assert(!VerifyTrackGain(bytes({0, 0}), bytes({0, 0})));
    assert(!VerifyTrackGain({}, {}));
    assert(!VerifyTrackGain(bytes(unity), bytes({1})));
    assert(!VerifyTrackGain(bytes({NAN}), bytes({NAN})));
    std::cout << "PCM_TRACK_HARNESS_ORACLES_PASS (portable adapters only)\n";
}
