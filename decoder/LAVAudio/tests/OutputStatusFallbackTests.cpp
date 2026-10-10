// SPDX-FileCopyrightText: 2026 OpenJOC contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Source-seam test: complete production queue/getter/Deliver/EOS methods and
// strict transaction/validation functions are extracted by the Python runner.
// These portable COM, AV-layout, allocator, clock and converter adapters are
// not the real Windows ABI, FFmpeg processing, decoder, renderer or hardware.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#endif

using HRESULT = std::int32_t;
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using WORD = std::uint16_t;
using BYTE = std::uint8_t;
using BOOL = int;
using GUID = int;
using REFERENCE_TIME = std::int64_t;
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_FAIL = -1, E_POINTER = -2,
                  E_UNEXPECTED = -3, E_OUTOFMEMORY = -4, E_INVALIDARG = -5,
                  VFW_E_TYPE_NOT_ACCEPTED = -6, VFW_E_UNSUPPORTED_AUDIO = -7,
                  VFW_E_BUFFER_UNDERFLOW = -8;
constexpr BOOL TRUE = 1, FALSE = 0;
constexpr int ERROR_ARITHMETIC_OVERFLOW = 534;
#define HRESULT_FROM_WIN32(x) (-static_cast<HRESULT>(x))
#define FAILED(hr) ((hr) < 0)
#define DbgLog(...) ((void)0)
#define FFSWAP(type, a, b) std::swap(a, b)
constexpr double DBL_SECOND_MULT = 10000000, PCM_BUFFER_MIN_DURATION = 200000,
                 PCM_BUFFER_MAX_DURATION = 1000000;
#ifndef FLT_EPSILON
constexpr double FLT_EPSILON = std::numeric_limits<float>::epsilon();
#endif
constexpr REFERENCE_TIME AV_NOPTS_VALUE = std::numeric_limits<REFERENCE_TIME>::min();
constexpr DWORD AV_CH_LAYOUT_5POINT1 = 0x60f, AV_CH_LAYOUT_5POINT1_BACK = 0x3f,
                AV_CH_LAYOUT_7POINT1 = 0x63f, JOC_MASK = 0x2d63f;
constexpr WORD WAVE_FORMAT_EXTENSIBLE = 0xfffe;
constexpr GUID MEDIATYPE_Audio = 1, MEDIASUBTYPE_IEEE_FLOAT = 2,
               FORMAT_WaveFormatEx = 3, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = 2;
enum LAVAudioSampleFormat { SampleFormat_16, SampleFormat_24, SampleFormat_32,
                           SampleFormat_U8, SampleFormat_FP32, SampleFormat_Bitstream };
const char *get_sample_format_desc(int format)
{
    switch (format)
    {
    case SampleFormat_16: return "16bit Integer";
    case SampleFormat_FP32: return "32bit Float";
    case SampleFormat_Bitstream: return "Bitstream";
    default: return "Other";
    }
}
#ifndef _MSC_VER
DWORD __popcnt(DWORD mask)
{
    DWORD result = 0;
    for (; mask; mask &= mask - 1) ++result;
    return result;
}
#endif
constexpr int AV_CHANNEL_ORDER_NATIVE = 1;
struct AVChannelLayout { int order = AV_CHANNEL_ORDER_NATIVE, nb_channels = 2; union { std::uint64_t mask; } u{3}; };
int av_channel_layout_compare(const AVChannelLayout *a, const AVChannelLayout *b)
{
    return a->order != b->order || a->nb_channels != b->nb_channels || a->u.mask != b->u.mask;
}
bool fail_layout_copy = false;
int av_channel_layout_copy(AVChannelLayout *a, const AVChannelLayout *b)
{
    if (fail_layout_copy) return -1;
    *a = *b;
    return 0;
}
void av_channel_layout_uninit(AVChannelLayout *a) { *a = {}; a->nb_channels = 0; a->u.mask = 0; }
void av_channel_layout_from_mask(AVChannelLayout *a, std::uint64_t mask)
{
    a->order = AV_CHANNEL_ORDER_NATIVE; a->u.mask = mask; a->nb_channels = __popcnt(static_cast<DWORD>(mask));
}
int av_channel_layout_check(const AVChannelLayout *a)
{
    return a->order == AV_CHANNEL_ORDER_NATIVE && static_cast<int>(__popcnt(static_cast<DWORD>(a->u.mask))) == a->nb_channels;
}
DWORD get_channel_mask(int n) { return n == 2 ? 3 : n == 6 ? AV_CH_LAYOUT_5POINT1 : n == 8 ? AV_CH_LAYOUT_7POINT1 : 0; }
#pragma pack(push, 1)
struct WAVEFORMATEX
{
    WORD wFormatTag = 3, nChannels = 2;
    DWORD nSamplesPerSec = 48000, nAvgBytesPerSec = 384000;
    WORD nBlockAlign = 8, wBitsPerSample = 32, cbSize = 0;
};
struct WAVEFORMATEXTENSIBLE
{
    WAVEFORMATEX Format;
    struct { WORD wValidBitsPerSample = 32; } Samples;
    DWORD dwChannelMask = 3;
    GUID SubFormat = MEDIASUBTYPE_IEEE_FLOAT;
};
#pragma pack(pop)
// Portable structural stand-in; no claim of Windows layout/ABI validation.
struct AM_MEDIA_TYPE
{
    GUID majortype = MEDIATYPE_Audio, subtype = MEDIASUBTYPE_IEEE_FLOAT, formattype = FORMAT_WaveFormatEx;
    BOOL bFixedSizeSamples = TRUE, bTemporalCompression = FALSE;
    ULONG lSampleSize = 8, cbFormat = sizeof(WAVEFORMATEXTENSIBLE);
    void *pUnk = nullptr;
    BYTE *pbFormat = nullptr;
    WAVEFORMATEXTENSIBLE wave{};
};
struct CMediaType : AM_MEDIA_TYPE
{
    CMediaType() { pbFormat = reinterpret_cast<BYTE *>(&wave); }
    CMediaType(const AM_MEDIA_TYPE &other) { *this = other; }
    CMediaType(const CMediaType &other) { *this = static_cast<const AM_MEDIA_TYPE &>(other); }
    CMediaType &operator=(const CMediaType &other) { return *this = static_cast<const AM_MEDIA_TYPE &>(other); }
    CMediaType &operator=(const AM_MEDIA_TYPE &other)
    {
        static_cast<AM_MEDIA_TYPE &>(*this) = other;
        if (other.pbFormat) std::memcpy(&wave, other.pbFormat, sizeof(wave));
        pbFormat = reinterpret_cast<BYTE *>(&wave);
        return *this;
    }
    void InitMediaType() { *this = CMediaType{}; }
    void SetType(const GUID *value) { majortype = *value; }
    void SetSubtype(const GUID *value) { subtype = *value; }
    void SetSampleSize(ULONG value) { lSampleSize = value; }
    void SetTemporalCompression(BOOL value) { bTemporalCompression = value; }
    void SetFormatType(const GUID *value) { formattype = *value; }
    bool SetFormat(const BYTE *data, ULONG size)
    {
        if (size != sizeof(wave)) return false;
        std::memcpy(&wave, data, size); cbFormat = size;
        return true;
    }
    const BYTE *Format() const { return reinterpret_cast<const BYTE *>(&wave); }
    BYTE *Format() { return reinterpret_cast<BYTE *>(&wave); }
    bool operator!=(const CMediaType &other) const
    {
        return subtype != other.subtype || wave.Format.nSamplesPerSec != other.wave.Format.nSamplesPerSec ||
               wave.Format.nChannels != other.wave.Format.nChannels || wave.dwChannelMask != other.wave.dwChannelMask ||
               wave.Format.wBitsPerSample != other.wave.Format.wBitsPerSample;
    }
};
LAVAudioSampleFormat sample_format(const AM_MEDIA_TYPE &mt)
{
    return mt.subtype == MEDIASUBTYPE_IEEE_FLOAT ? SampleFormat_FP32 : SampleFormat_16;
}
void DeleteMediaType(AM_MEDIA_TYPE *type) { delete static_cast<CMediaType *>(type); }
struct LAVOpenJocOutputContract
{
    int policy;
    DWORD channel_count, windows_channel_mask;
    std::uint64_t ffmpeg_channel_mask;
    const char *property_page_label;
};
const LAVOpenJocOutputContract joc_contract{1, 12, JOC_MASK, JOC_MASK, "7.1.4"};
const LAVOpenJocOutputContract *FindLAVOpenJocOutputContract(int policy) { return policy == 1 ? &joc_contract : nullptr; }
struct LAVOpenJocStrictMediaType
{
    GUID major_type{}, subtype{};
    BOOL fixed_size_samples = FALSE, temporal_compression = FALSE;
    GUID format_type{};
    ULONG format_size = 0;
    WAVEFORMATEXTENSIBLE wave{};
    ULONG sample_size = 0;
};
struct LAVOpenJocStrictAcquiredSample
{
    void *handle = nullptr; BYTE *data = nullptr; AM_MEDIA_TYPE *attached_type = nullptr; long capacity = 0;
};
struct LAVOpenJocStrictDeliveryOperations
{
    std::function<HRESULT()> prepare_delivery;
    std::function<HRESULT(const AM_MEDIA_TYPE &)> query_accept;
    std::function<HRESULT(long, const AM_MEDIA_TYPE &)> reconnect;
    std::function<HRESULT(LAVOpenJocStrictAcquiredSample *)> acquire_sample;
    std::function<void(AM_MEDIA_TYPE *)> release_attached_type;
    std::function<void(void *)> release_sample;
    std::function<HRESULT(void *, const AM_MEDIA_TYPE &)> set_sample_media_type;
    std::function<HRESULT(const AM_MEDIA_TYPE &)> set_output_media_type;
    std::function<HRESULT(void *, BYTE *, long)> deliver;
};
struct LAVOpenJocQueueTransactionInput
{
    bool compatible; std::uint32_t queued_samples, incoming_samples;
    REFERENCE_TIME queued_start, incoming_start; std::uint32_t sample_rate;
};
struct LAVOpenJocQueueTransactionResult { std::uint32_t sample_count = 0; REFERENCE_TIME start_time = 0; };
struct LAVOpenJocQueueTransactionOperations
{
    std::function<HRESULT()> flush, prepare_metadata;
    std::function<void()> swap_buffer;
    std::function<HRESULT()> append_buffer;
};
template<class T> struct GrowableArray
{
    std::vector<T> data;
    bool fail_append = false;
    HRESULT Append(GrowableArray<T> *other)
    {
        if (fail_append) return E_OUTOFMEMORY;
        data.insert(data.end(), other->data.begin(), other->data.end()); return S_OK;
    }
    void SetSize(std::size_t n) { data.resize(n); }
    std::size_t GetCount() const { return data.size(); }
    T *Ptr() { return data.data(); }
};
struct BufferDetails
{
    GrowableArray<BYTE> *bBuffer = new GrowableArray<BYTE>;
    LAVAudioSampleFormat sfFormat = SampleFormat_FP32;
    WORD wBitsPerSample = 32;
    DWORD dwSamplesPerSec = 44100;
    std::uint32_t nSamples = 0;
    AVChannelLayout layout{};
    REFERENCE_TIME rtStart = 0;
    BOOL bPlanar = FALSE;
    const LAVOpenJocOutputContract *openjoc_contract = nullptr;
    BufferDetails() = default;
    BufferDetails(const BufferDetails &) = delete;
    BufferDetails &operator=(const BufferDetails &) = delete;
    ~BufferDetails() { delete bBuffer; }
};
struct IMediaSample
{
    std::vector<BYTE> bytes = std::vector<BYTE>(65536);
    CMediaType type;
    bool attached = false;
    long length = 0;
    static int live;
    IMediaSample() { ++live; }
    HRESULT SetMediaType(AM_MEDIA_TYPE *value) { type = *value; attached = true; return S_OK; }
    HRESULT GetPointer(BYTE **value) { *value = bytes.data(); return S_OK; }
    long GetSize() const { return static_cast<long>(bytes.size()); }
    HRESULT GetMediaType(AM_MEDIA_TYPE **value) { *value = nullptr; return S_FALSE; }
    void SetTime(REFERENCE_TIME *, REFERENCE_TIME *) {}
    void SetMediaTime(void *, void *) {}
    void SetPreroll(BOOL) {}
    void SetDiscontinuity(BOOL) {}
    void SetSyncPoint(BOOL) {}
    HRESULT SetActualDataLength(long n) { length = n; return S_OK; }
    void Release() { --live; delete this; }
};
int IMediaSample::live = 0;
struct OutputPin
{
    CMediaType current, delivered_type;
    bool connected = true;
    int query_calls = 0, deliveries = 0, accepted_deliveries = 0;
    HRESULT delivery_result = S_OK, acquire_result = S_OK;
    std::vector<BYTE> delivered_bytes;
    std::function<HRESULT(const AM_MEDIA_TYPE &)> query = [](const AM_MEDIA_TYPE &) { return S_OK; };
    std::function<void()> on_delivery;
    BOOL IsConnected() const { return connected; }
    CMediaType &CurrentMediaType() { return current; }
    OutputPin *GetConnected() { return this; }
    HRESULT QueryAccept(const AM_MEDIA_TYPE *type) { ++query_calls; return query(*type); }
    HRESULT SetMediaType(CMediaType *type) { current = *type; return S_OK; }
    HRESULT GetDeliveryBuffer(IMediaSample **sample, void *, void *, int)
    {
        if (FAILED(acquire_result)) return acquire_result;
        *sample = new IMediaSample; return S_OK;
    }
    HRESULT Deliver(IMediaSample *sample)
    {
        ++deliveries;
        if (on_delivery) on_delivery();
        delivered_type = sample->attached ? sample->type : current;
        delivered_bytes.assign(sample->bytes.begin(), sample->bytes.begin() + sample->length);
        if (delivery_result == S_OK) ++accepted_deliveries;
        return delivery_result;
    }
};
template<class T> void SafeRelease(T **value) { if (*value) (*value)->Release(); *value = nullptr; }
struct Jitter { void Sample(REFERENCE_TIME) {} REFERENCE_TIME AbsMinimum() const { return 0; } void OffsetValues(REFERENCE_TIME) {} };
struct CAutoLock
{
    std::recursive_mutex *lock;
    explicit CAutoLock(std::recursive_mutex *value) : lock(value) { lock->lock(); }
    ~CAutoLock() { lock->unlock(); }
};
enum class LAVOpenJocState { Idle, OpenJoc };
enum class LAVOpenJocFailureReason { UnsupportedOutputLayout };
struct JocState
{
    LAVOpenJocState state = LAVOpenJocState::Idle;
    int diagnostics = 0;
    LAVOpenJocState State() const { return state; }
    void RecordRuntimeDiagnostic(LAVOpenJocFailureReason, const char *) { ++diagnostics; }
};
struct MockBase
{
    int eos_calls = 0;
    HRESULT EndOfStream() { ++eos_calls; return S_OK; }
};
// MSVC's __super is a keyword; GCC/Clang use this token macro. The extracted
// EndOfStream body itself is unchanged and calls the adapter base class.
#ifndef _MSC_VER
#define __super MockBase
#endif
// Deliberately deterministic byte transform, not float/integer PCM conversion.
// It provides an unchanged-payload oracle across pre-fix and fixed executions.
std::vector<BYTE> mock_converted_bytes(const std::vector<BYTE> &input, std::size_t output_size)
{
    std::vector<BYTE> output(output_size);
    for (std::size_t i = 0; i < output_size; ++i)
        output[i] = static_cast<BYTE>(input[(i * 3 + 7) % input.size()] ^ static_cast<BYTE>((i * 17 + 0x5a) & 0xff));
    return output;
}
struct CLAVAudio : MockBase
{
    OutputPin output;
    OutputPin *m_pOutput = &output;
    void *m_avBSContext = nullptr;
    BufferDetails m_OutputQueue;
    std::atomic<int> m_outputStatusFormat{SampleFormat_FP32}, m_outputStatusChannels{12},
                     m_outputStatusSampleRate{48000}, m_volumeStatsChannels{12};
    std::atomic<DWORD> m_outputStatusChannelMask{JOC_MASK};
    BOOL m_bFlushing = FALSE, m_bResyncTimestamp = FALSE, m_bDiscontinuity = FALSE,
         m_bMixingSettingsChanged = FALSE;
    REFERENCE_TIME m_rtStart = 0, m_JitterLimit = 1000000;
    double m_dRate = 1, m_dStartOffset = 0;
    Jitter m_faJitter;
    struct Settings { BOOL AutoAVSync = FALSE, AudioDelayEnabled = FALSE; int AudioDelay = 0; } m_settings;
    LAVAudioSampleFormat m_FallbackFormat = SampleFormat_FP32;
    AVChannelLayout m_chOverrideMixer{};
    std::recursive_mutex m_csReceive;
    JocState m_openJoc;
    int converter_calls = 0, process_calls = 0, admission_refreshes = 0;
    HRESULT reconnect_override = S_OK, process_result = S_OK;
    bool force_reconnect_result = false;
    HRESULT QueueOutput(BufferDetails &);
    HRESULT FlushOutput(BOOL deliver = TRUE);
    HRESULT FlushOutputLocked(BOOL);
    HRESULT GetOutputDetails(const char **, int *, int *, DWORD *);
    HRESULT Deliver(BufferDetails &);
    HRESULT PrepareOpenJocDelivery(BufferDetails &, REFERENCE_TIME &, REFERENCE_TIME &);
    HRESULT CompleteOpenJocDelivery(BufferDetails &, IMediaSample *, BYTE *, long, REFERENCE_TIME, REFERENCE_TIME);
    HRESULT EndOfStream();
    HRESULT ProcessBuffer(void *, BOOL = FALSE) { ++process_calls; return process_result; }
    void RefreshOpenJocAdmissionSnapshot() { ++admission_refreshes; }
    CMediaType CreateMediaType(LAVAudioSampleFormat sf, DWORD rate, WORD channels, DWORD mask, WORD bits)
    {
        CMediaType mt;
        mt.subtype = sf == SampleFormat_FP32 ? MEDIASUBTYPE_IEEE_FLOAT : 4;
        mt.wave.Format.nChannels = channels; mt.wave.Format.nSamplesPerSec = rate;
        mt.wave.Format.wBitsPerSample = bits ? bits : (sf == SampleFormat_16 ? 16 : 32);
        mt.wave.Format.nBlockAlign = channels * mt.wave.Format.wBitsPerSample / 8;
        mt.wave.Format.nAvgBytesPerSec = rate * mt.wave.Format.nBlockAlign;
        mt.wave.Format.wFormatTag = channels > 2 ? WAVE_FORMAT_EXTENSIBLE : (sf == SampleFormat_FP32 ? 3 : 1);
        mt.wave.dwChannelMask = mask; mt.wave.SubFormat = mt.subtype;
        return mt;
    }
    HRESULT ReconnectOutput(long, CMediaType &mt)
    {
        return force_reconnect_result ? reconnect_override : (mt != output.current ? S_OK : S_FALSE);
    }
    HRESULT GetDeliveryBuffer(IMediaSample **sample, BYTE **data)
    {
        HRESULT hr = output.GetDeliveryBuffer(sample, nullptr, nullptr, 0);
        if (FAILED(hr)) return hr;
        return (*sample)->GetPointer(data);
    }
    HRESULT PerformAVRProcessing(BufferDetails *buffer)
    {
        ++converter_calls;
        const auto input = buffer->bBuffer->data;
        buffer->sfFormat = m_FallbackFormat;
        buffer->wBitsPerSample = buffer->sfFormat == SampleFormat_16 ? 16 : 32;
        if (m_bMixingSettingsChanged) buffer->layout = m_chOverrideMixer;
        const std::size_t size = buffer->nSamples * buffer->layout.nb_channels * (buffer->wBitsPerSample / 8);
        buffer->bBuffer->data = mock_converted_bytes(input, size);
        return S_OK;
    }
};
#include "OutputStatusProductionMethods.inc"

namespace {
int failures = 0, cases = 0;
std::string test_name;
void check(bool condition, const char *message)
{
    if (!condition) { ++failures; std::cerr << "FAIL " << test_name << ": " << message << '\n'; }
}
void begin(const std::string &name) { test_name = name; ++cases; }
void fill(BufferDetails &buffer, int channels = 2, DWORD mask = 3, DWORD rate = 44100,
          const LAVOpenJocOutputContract *contract = nullptr, unsigned samples = 64)
{
    buffer.nSamples = samples; buffer.dwSamplesPerSec = rate; buffer.layout.nb_channels = channels;
    buffer.layout.u.mask = mask; buffer.openjoc_contract = contract;
    buffer.bBuffer->SetSize(samples * channels * 4);
    for (std::size_t i = 0; i < buffer.bBuffer->GetCount(); ++i)
        buffer.bBuffer->data[i] = static_cast<BYTE>((i * 29 + (i >> 3) * 11 + 0x31) & 0xff);
}
struct Status { std::string format; int channels = 0, rate = 0; DWORD mask = 0; };
Status status(CLAVAudio &audio)
{
    const char *format = nullptr; Status result;
    check(audio.GetOutputDetails(&format, &result.channels, &result.rate, &result.mask) == S_OK, "getter result");
    result.format = format ? format : "null"; return result;
}
void check_status(CLAVAudio &audio, LAVAudioSampleFormat format, int channels, int rate, DWORD mask)
{
    const Status value = status(audio);
    check(value.format == get_sample_format_desc(format), "output format");
    check(value.channels == channels, "output channels"); check(value.rate == rate, "output sample rate");
    check(value.mask == mask, "output mask");
    check(audio.m_volumeStatsChannels.load() == channels, "volume channel bound");
}
void check_empty(CLAVAudio &audio)
{
    check(audio.m_OutputQueue.nSamples == 0 && audio.m_OutputQueue.bBuffer->GetCount() == 0 &&
          audio.m_OutputQueue.rtStart == AV_NOPTS_VALUE && audio.m_OutputQueue.openjoc_contract == nullptr,
          "queue fully cleared");
    check(IMediaSample::live == 0, "sample released");
}
void payload(CLAVAudio &audio, const std::vector<BYTE> &expected)
{
    check(audio.output.delivered_bytes == expected, "exact expected copied/mock-converted bytes");
    std::uint64_t hash = 14695981039346656037ULL;
    for (BYTE b : audio.output.delivered_bytes) { hash ^= b; hash *= 1099511628211ULL; }
    std::cout << "PAYLOAD " << test_name << " bytes=" << audio.output.delivered_bytes.size()
              << " fnv1a=" << std::hex << hash << std::dec << '\n';
}
void previous_type(CLAVAudio &audio, bool joc)
{
    audio.output.current = audio.CreateMediaType(SampleFormat_FP32, 48000, joc ? 12 : 2, joc ? JOC_MASK : 3, 32);
}
void fallback_query(CLAVAudio &audio)
{
    audio.output.query = [](const AM_MEDIA_TYPE &type) { return sample_format(type) == SampleFormat_16 ? S_OK : S_FALSE; };
}
void final_flush_matrix()
{
    for (bool prior_joc : {false, true}) for (bool fallback : {false, true}) for (bool eos : {false, true})
    {
        begin(std::string("final-") + (eos ? "eos" : "flush") + (prior_joc ? "-prior-joc" : "-prior-pcm") +
              (fallback ? "-16bit-fallback" : "-float"));
        CLAVAudio audio; previous_type(audio, prior_joc); if (fallback) fallback_query(audio);
        BufferDetails buffer; fill(buffer); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK, "queue succeeds");
        check(audio.output.deliveries == 0, "short final block remains queued");
        check_status(audio, SampleFormat_FP32, 2, 44100, 3);
        audio.output.on_delivery = [&audio]() { check_status(audio, SampleFormat_FP32, 2, 44100, 3); };
        check((eos ? audio.EndOfStream() : audio.FlushOutputLocked(TRUE)) == S_OK, "final delivery succeeds");
        check(audio.output.accepted_deliveries == 1, "one accepted delivery");
        check_status(audio, fallback ? SampleFormat_16 : SampleFormat_FP32, 2, 44100, 3);
        payload(audio, fallback ? mock_converted_bytes(input, input.size() / 2) : input);
        check(audio.converter_calls == (fallback ? 1 : 0), "converter invocation count");
        check_empty(audio);
        check(audio.FlushOutputLocked(TRUE) == S_OK, "empty repeat flush succeeds");
        check_status(audio, fallback ? SampleFormat_16 : SampleFormat_FP32, 2, 44100, 3);
        if (eos) check(audio.process_calls == 2 && audio.admission_refreshes == 2 && audio.eos_calls == 1, "full EOS seam");
    }
}
void unsuccessful_delivery()
{
    for (HRESULT result : {S_FALSE, E_FAIL})
    {
        begin(result == S_FALSE ? "delivery-S_FALSE" : "delivery-failure");
        CLAVAudio audio; previous_type(audio, false); fallback_query(audio); audio.output.delivery_result = result;
        BufferDetails buffer; fill(buffer); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK, "queue succeeds");
        check(audio.FlushOutputLocked(TRUE) == result, "delivery result propagated");
        check_status(audio, SampleFormat_FP32, 2, 44100, 3);
        check(audio.output.accepted_deliveries == 0, "no accepted delivery");
        payload(audio, mock_converted_bytes(input, input.size() / 2)); check_empty(audio);
    }
    for (bool reconnect : {false, true})
    {
        begin(reconnect ? "reconnect-failure" : "sample-acquire-failure");
        CLAVAudio audio; previous_type(audio, false);
        if (reconnect) { audio.force_reconnect_result = true; audio.reconnect_override = E_FAIL; }
        else audio.output.acquire_result = E_FAIL;
        BufferDetails buffer; fill(buffer); check(audio.QueueOutput(buffer) == S_OK, "queue succeeds");
        check(audio.FlushOutputLocked(TRUE) == E_FAIL, "failure propagated");
        check_status(audio, SampleFormat_FP32, 2, 44100, 3);
        check(audio.output.deliveries == 0 && audio.converter_calls == 0, "no delivery or conversion"); check_empty(audio);
    }
    begin("flushing-S_FALSE");
    CLAVAudio audio; BufferDetails buffer; fill(buffer); audio.m_bFlushing = TRUE;
    check(audio.QueueOutput(buffer) == S_OK, "queue succeeds");
    check(audio.FlushOutputLocked(TRUE) == S_FALSE, "flushing propagated");
    check_status(audio, SampleFormat_FP32, 2, 44100, 3); check_empty(audio);
}
void ordinary_negotiation_edges()
{
    begin("no-new-media-type");
    {
        CLAVAudio audio; audio.output.current = audio.CreateMediaType(SampleFormat_FP32, 44100, 2, 3, 32);
        audio.output.query = [](const AM_MEDIA_TYPE &) { return E_FAIL; };
        BufferDetails buffer; fill(buffer); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK && audio.FlushOutputLocked(TRUE) == S_OK, "delivery succeeds");
        check(audio.output.query_calls == 0 && audio.converter_calls == 0, "unchanged type bypasses negotiation");
        check_status(audio, SampleFormat_FP32, 2, 44100, 3); payload(audio, input); check_empty(audio);
    }
    for (HRESULT rejection : {S_FALSE, E_FAIL})
    {
        begin(rejection == S_FALSE ? "all-candidates-rejected" : "all-query-accept-failed");
        CLAVAudio audio; previous_type(audio, false); audio.output.query = [rejection](const AM_MEDIA_TYPE &) { return rejection; };
        BufferDetails buffer; fill(buffer); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK && audio.FlushOutputLocked(TRUE) == S_OK, "existing ordinary branch still attempts delivery");
        check(audio.output.query_calls == 2 && audio.converter_calls == 0, "both candidates rejected without conversion");
        check(audio.output.deliveries == 1, "existing ordinary rejection behavior preserved");
        // Existing ordinary code attaches its last rejected 16-bit proposal, but
        // leaves the FP32 buffer unchanged. Do not claim a negotiated agreement.
        check(sample_format(audio.output.delivered_type) == SampleFormat_16, "last rejected proposal still attached");
        check_status(audio, SampleFormat_FP32, 2, 44100, 3); payload(audio, input); check_empty(audio);
    }
    begin("5point1-back-mask-fallback");
    {
        CLAVAudio audio; previous_type(audio, false);
        audio.output.query = [](const AM_MEDIA_TYPE &type) { return type.wave.dwChannelMask == AV_CH_LAYOUT_5POINT1_BACK ? S_OK : S_FALSE; };
        BufferDetails buffer; fill(buffer, 6, AV_CH_LAYOUT_5POINT1); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK, "queue succeeds");
        check_status(audio, SampleFormat_FP32, 6, 44100, AV_CH_LAYOUT_5POINT1);
        check(audio.FlushOutputLocked(TRUE) == S_OK, "mask fallback succeeds");
        check_status(audio, SampleFormat_FP32, 6, 44100, AV_CH_LAYOUT_5POINT1_BACK);
        check(audio.converter_calls == 0 && audio.output.query_calls == 3, "mask-only negotiation");
        payload(audio, input); check_empty(audio);
    }
    for (bool retain_stereo : {false, true})
    {
        begin(retain_stereo ? "retain-current-stereo-layout" : "12-to-8-channel-fallback");
        CLAVAudio audio; previous_type(audio, false);
        const int channels = retain_stereo ? 2 : 8; const DWORD mask = retain_stereo ? 3 : AV_CH_LAYOUT_7POINT1;
        audio.output.query = [channels](const AM_MEDIA_TYPE &type) {
            return type.wave.Format.nChannels == channels && sample_format(type) == SampleFormat_FP32 ? S_OK : S_FALSE;
        };
        BufferDetails buffer; fill(buffer, 12, JOC_MASK); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK, "queue succeeds");
        check_status(audio, SampleFormat_FP32, 12, 44100, JOC_MASK);
        audio.output.on_delivery = [&audio]() { check_status(audio, SampleFormat_FP32, 12, 44100, JOC_MASK); };
        check(audio.FlushOutputLocked(TRUE) == S_OK, "channel fallback succeeds");
        check_status(audio, SampleFormat_FP32, channels, 44100, mask);
        check(audio.converter_calls == 1 && audio.m_bMixingSettingsChanged, "one layout-conversion adapter call");
        payload(audio, mock_converted_bytes(input, 64 * channels * 4)); check_empty(audio);
    }
}
void reset_and_getter_edges()
{
    begin("discard-and-empty-flush");
    CLAVAudio audio; fallback_query(audio); BufferDetails buffer; fill(buffer);
    check(audio.QueueOutput(buffer) == S_OK && audio.FlushOutput(FALSE) == S_OK, "discard succeeds");
    check(audio.output.deliveries == 0 && audio.converter_calls == 0, "discard does not negotiate/deliver");
    check_status(audio, SampleFormat_FP32, 2, 44100, 3); check_empty(audio);
    check(audio.FlushOutput(TRUE) == S_OK, "empty flush"); check_status(audio, SampleFormat_FP32, 2, 44100, 3);
    begin("getter-pointer-and-bitstream-controls");
    check(audio.GetOutputDetails(nullptr, nullptr, nullptr, nullptr) == S_OK, "optional getter pointers");
    audio.output.connected = false;
    check(audio.GetOutputDetails(nullptr, nullptr, nullptr, nullptr) == E_UNEXPECTED, "disconnected getter");
    audio.output.connected = true; audio.m_pOutput = nullptr;
    check(audio.GetOutputDetails(nullptr, nullptr, nullptr, nullptr) == E_UNEXPECTED, "null output getter");
    audio.m_pOutput = &audio.output; audio.m_avBSContext = &audio;
    const char *format = nullptr; int channels = 91, rate = 92; DWORD mask = 93;
    check(audio.GetOutputDetails(&format, &channels, &rate, &mask) == S_FALSE, "bitstream getter");
    check(std::string(format) == "Bitstream" && channels == 91 && rate == 92 && mask == 93, "bitstream leaves PCM details untouched");
    begin("non-native-status-mask");
    CLAVAudio non_native; BufferDetails other; fill(other); other.layout.order = 2;
    check(non_native.QueueOutput(other) == S_OK && non_native.FlushOutputLocked(TRUE) == S_OK, "non-native ordinary seam");
    check_status(non_native, SampleFormat_FP32, 2, 44100, 0); check_empty(non_native);
    begin("negative-timestamp-preroll");
    CLAVAudio preroll; previous_type(preroll, false); BufferDetails negative; fill(negative);
    check(preroll.QueueOutput(negative) == S_OK, "queue succeeds"); preroll.m_rtStart = -1;
    check(preroll.FlushOutputLocked(TRUE) == S_OK && preroll.output.deliveries == 0, "existing ordinary negative timestamp skip");
    check_status(preroll, SampleFormat_FP32, 2, 44100, 3); check_empty(preroll);
}
void queue_transaction_controls()
{
    begin("compatible-queue-append");
    CLAVAudio audio; BufferDetails first, second; fill(first); fill(second);
    auto expected = first.bBuffer->data; expected.insert(expected.end(), second.bBuffer->data.begin(), second.bBuffer->data.end());
    first.rtStart = AV_NOPTS_VALUE; second.rtStart = 20000;
    check(audio.QueueOutput(first) == S_OK && audio.QueueOutput(second) == S_OK, "compatible append");
    check(audio.m_OutputQueue.nSamples == 128 && audio.m_OutputQueue.bBuffer->data == expected, "real append transaction payload");
    check(audio.m_OutputQueue.rtStart == 20000 - static_cast<REFERENCE_TIME>(64.0 / 44100 * 10000000.0), "real merged timestamp");
    check(audio.FlushOutput(TRUE) == S_OK, "append delivery"); payload(audio, expected); check_empty(audio);
    begin("queue-checked-add-overflow");
    CLAVAudio overflow; BufferDetails empty, incoming; fill(empty); fill(incoming);
    check(overflow.QueueOutput(empty) == S_OK, "initial queue");
    overflow.m_OutputQueue.nSamples = std::numeric_limits<std::uint32_t>::max();
    const auto before = overflow.m_OutputQueue.bBuffer->data;
    check(overflow.QueueOutput(incoming) == HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW), "real checked add rejects overflow");
    check(overflow.m_OutputQueue.bBuffer->data == before && incoming.nSamples == 64, "overflow keeps buffers unconsumed");
    check_status(overflow, SampleFormat_FP32, 2, 44100, 3); check(overflow.FlushOutput(FALSE) == S_OK, "overflow discard");
    begin("queue-append-failure");
    CLAVAudio append; BufferDetails a, b; fill(a); fill(b); check(append.QueueOutput(a) == S_OK, "initial queue");
    append.m_OutputQueue.bBuffer->fail_append = true;
    check(append.QueueOutput(b) == E_OUTOFMEMORY && b.nSamples == 64 && append.m_OutputQueue.nSamples == 64, "append failure transaction");
    check(append.FlushOutput(FALSE) == S_OK, "append discard");
    begin("queue-metadata-copy-failure");
    CLAVAudio copy; BufferDetails c; fill(c, 6, AV_CH_LAYOUT_5POINT1); fail_layout_copy = true;
    check(copy.QueueOutput(c) == E_OUTOFMEMORY && c.nSamples == 64, "metadata failure transaction");
    fail_layout_copy = false; check_status(copy, SampleFormat_FP32, 12, 48000, JOC_MASK);
}
void incompatible_queue_transitions()
{
    for (HRESULT result : {S_OK, S_FALSE, E_FAIL})
    {
        begin("ordinary-to-strict-queue-delivery-" + std::to_string(result));
        CLAVAudio audio; previous_type(audio, false); fallback_query(audio); audio.output.delivery_result = result;
        BufferDetails ordinary, strict; fill(ordinary); fill(strict, 12, JOC_MASK, 48000, &joc_contract);
        const auto ordinary_bytes = ordinary.bBuffer->data; const auto strict_bytes = strict.bBuffer->data;
        check(audio.QueueOutput(ordinary) == S_OK, "ordinary short block queued");
        check(audio.QueueOutput(strict) == (FAILED(result) ? result : S_OK), "real incompatible transition result");
        payload(audio, mock_converted_bytes(ordinary_bytes, ordinary_bytes.size() / 2));
        check(audio.converter_calls == 1 && audio.output.deliveries == 1, "only preceding ordinary buffer converted/delivered");
        if (FAILED(result))
        {
            check(strict.nSamples == 64 && strict.bBuffer->data == strict_bytes && strict.openjoc_contract == &joc_contract,
                  "failed old delivery leaves incoming strict buffer unconsumed");
            check_status(audio, SampleFormat_FP32, 2, 44100, 3); check_empty(audio);
        }
        else
        {
            // The existing queue transaction regards S_FALSE as non-failure.
            // Its behavior is preserved; no successful old delivery is claimed.
            check(strict.nSamples == 0 && audio.m_OutputQueue.openjoc_contract == &joc_contract &&
                  audio.m_OutputQueue.bBuffer->data == strict_bytes, "strict queue transaction owns unchanged strict payload");
            check_status(audio, SampleFormat_FP32, 12, 48000, JOC_MASK);
            check(audio.FlushOutput(FALSE) == S_OK, "discard strict pending block"); check_empty(audio);
        }
    }
    begin("strict-to-ordinary-queue-transition");
    CLAVAudio audio; previous_type(audio, false); BufferDetails strict, ordinary;
    fill(strict, 12, JOC_MASK, 48000, &joc_contract); fill(ordinary);
    const auto strict_bytes = strict.bBuffer->data, ordinary_bytes = ordinary.bBuffer->data;
    check(audio.QueueOutput(strict) == S_OK && audio.QueueOutput(ordinary) == S_OK, "strict old block flushes before ordinary queue");
    payload(audio, strict_bytes);
    check(audio.output.deliveries == 1 && audio.converter_calls == 0, "strict bytes unchanged before ordinary transition");
    check(audio.m_OutputQueue.openjoc_contract == nullptr && audio.m_OutputQueue.bBuffer->data == ordinary_bytes,
          "incoming ordinary payload now queued");
    check_status(audio, SampleFormat_FP32, 2, 44100, 3);
    check(audio.FlushOutput(FALSE) == S_OK, "discard ordinary pending block"); check_empty(audio);
}
void strict_controls()
{
    for (HRESULT query_result : {S_OK, S_FALSE, E_FAIL}) for (HRESULT delivery_result : {S_OK, S_FALSE, E_FAIL})
    {
        begin("strict-query-" + std::to_string(query_result) + "-delivery-" + std::to_string(delivery_result));
        CLAVAudio audio; previous_type(audio, false); audio.output.query = [query_result](const AM_MEDIA_TYPE &) { return query_result; };
        audio.output.delivery_result = delivery_result; BufferDetails buffer; fill(buffer, 12, JOC_MASK, 48000, &joc_contract);
        const auto input = buffer.bBuffer->data; const CMediaType previous = audio.output.current;
        check(audio.QueueOutput(buffer) == S_OK, "strict queue");
        const HRESULT expected = query_result == S_FALSE ? VFW_E_TYPE_NOT_ACCEPTED : query_result == E_FAIL ? E_FAIL : delivery_result;
        check(audio.FlushOutputLocked(TRUE) == expected, "strict result propagation");
        check_status(audio, SampleFormat_FP32, 12, 48000, JOC_MASK);
        check(audio.converter_calls == 0, "strict never ordinary fallback");
        if (query_result == S_OK) payload(audio, input); else check(audio.output.deliveries == 0, "strict rejected negotiation does not deliver");
        if (query_result == S_OK && delivery_result == S_OK)
            check(IsExactLAVOpenJocStrictMediaType(joc_contract, audio.output.current), "strict type committed on success");
        else check(!(audio.output.current != previous), "strict transition remains pending");
        check_empty(audio);
    }
    begin("strict-no-new-type");
    {
        CLAVAudio audio; check(CreateOpenJocStrictDirectShowMediaType(joc_contract, &audio.output.current) == S_OK, "strict current type");
        BufferDetails buffer; fill(buffer, 12, JOC_MASK, 48000, &joc_contract); const auto input = buffer.bBuffer->data;
        check(audio.QueueOutput(buffer) == S_OK && audio.EndOfStream() == S_OK, "strict EOS success");
        check(audio.output.query_calls == 0 && audio.eos_calls == 1, "strict unchanged type bypass");
        payload(audio, input); check_status(audio, SampleFormat_FP32, 12, 48000, JOC_MASK); check_empty(audio);
    }
    for (int invalid : {0, 1, 2})
    {
        begin("strict-invalid-buffer-" + std::to_string(invalid));
        CLAVAudio audio; BufferDetails buffer; fill(buffer, 12, JOC_MASK, 48000, &joc_contract);
        if (invalid == 0) buffer.bPlanar = TRUE;
        if (invalid == 1) buffer.dwSamplesPerSec = 44100;
        if (invalid == 2) buffer.bBuffer->SetSize(buffer.bBuffer->GetCount() - 1);
        check(audio.QueueOutput(buffer) == S_OK, "strict malformed buffer queued at seam");
        // QueueOutput assumes processed/interleaved input and does not copy the
        // planar flag. Inject it at the delivery seam to test real validation.
        if (invalid == 0) audio.m_OutputQueue.bPlanar = TRUE;
        check(audio.FlushOutputLocked(TRUE) == E_INVALIDARG && audio.output.deliveries == 0, "real strict validator rejects before delivery");
        check(audio.converter_calls == 0, "strict invalid does not fallback"); check_empty(audio);
    }
    begin("strict-eos-process-error");
    CLAVAudio strict_eos; strict_eos.m_openJoc.state = LAVOpenJocState::OpenJoc; strict_eos.process_result = E_FAIL;
    check(strict_eos.EndOfStream() == E_FAIL && strict_eos.eos_calls == 0 && strict_eos.process_calls == 1, "real strict EOS error propagation");
    begin("ordinary-eos-process-error-control");
    CLAVAudio ordinary_eos; ordinary_eos.process_result = E_FAIL;
    check(ordinary_eos.EndOfStream() == S_OK && ordinary_eos.eos_calls == 1 && ordinary_eos.process_calls == 2, "existing ordinary EOS error normalization");
}
} // namespace
int main()
{
    final_flush_matrix(); unsuccessful_delivery(); ordinary_negotiation_edges();
    reset_and_getter_edges(); queue_transaction_controls(); incompatible_queue_transitions(); strict_controls();
    check(IMediaSample::live == 0, "all media samples released");
    std::cout << "Output status source seam: " << cases << " cases, " << failures << " failures\n";
    return failures ? 1 : 0;
}
