/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
// Portable adapters for the unmodified admission, mapping and initialization
// methods. This is not evidence of native COM or real FFmpeg execution.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using UINT = unsigned;
using HRESULT = int;
struct GUID
{
    std::uint32_t value;
    std::uint8_t padding[12] = {};
    bool operator==(const GUID &other) const { return value == other.value; }
    bool operator!=(const GUID &other) const { return !(*this == other); }
};
constexpr GUID MEDIATYPE_Audio{1}, MEDIASUBTYPE_PCM{2}, MEDIASUBTYPE_IEEE_FLOAT{3},
    FORMAT_WaveFormatEx{4}, FORMAT_WaveFormatExFFMPEG{5}, FORMAT_VorbisFormat2{6},
    MEDIASUBTYPE_DOLBY_AC3_SPDIF{7}, MEDIASUBTYPE_FFMPEG_AUDIO{8},
    MEDIATYPE_DVD_ENCRYPTED_PACK{9}, MEDIATYPE_MPEG2_PACK{10}, MEDIATYPE_MPEG2_PES{11},
    MEDIASUBTYPE_FLAC{12}, MEDIASUBTYPE_DOLBY_DDPLUS{13};
constexpr WORD WAVE_FORMAT_PCM = 1, WAVE_FORMAT_IEEE_FLOAT = 3, WAVE_FORMAT_EXTENSIBLE = 0xfffe;
#pragma pack(push, 1)
struct WAVEFORMATEX
{
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct WAVEFORMATEXTENSIBLE
{
    WAVEFORMATEX Format;
    union { WORD wValidBitsPerSample; } Samples;
    DWORD dwChannelMask;
    GUID SubFormat;
};
struct WAVEFORMATEXFFMPEG { WAVEFORMATEX wfex; int nCodecId; };
#pragma pack(pop)
static_assert(sizeof(WAVEFORMATEX) == 18 && sizeof(WAVEFORMATEXTENSIBLE) == 40, "Wave ABI");
struct CMediaType
{
    GUID majortype = MEDIATYPE_Audio, subtype = MEDIASUBTYPE_PCM, formattype = FORMAT_WaveFormatEx;
    DWORD cbFormat = 0;
    BYTE *pbFormat = nullptr;
    BYTE *Format() const { return pbFormat; }
};
enum AVCodecID
{
    AV_CODEC_ID_NONE, AV_CODEC_ID_PCM_U8, AV_CODEC_ID_PCM_S16LE,
    AV_CODEC_ID_PCM_S24LE, AV_CODEC_ID_PCM_S32LE, AV_CODEC_ID_PCM_F32LE,
    AV_CODEC_ID_PCM_F64LE, AV_CODEC_ID_AC3, AV_CODEC_ID_FLAC, AV_CODEC_ID_EAC3
};
#include "OpenJocPcmInput.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define DbgLog(x) ((void)0)
#define FAILED(x) ((x) < 0)
#define __super Base
constexpr HRESULT S_OK = 0, VFW_E_TYPE_NOT_ACCEPTED = -1, VFW_E_UNSUPPORTED_AUDIO = -2;
enum PIN_DIRECTION { PINDIR_INPUT, PINDIR_OUTPUT };
struct AMOVIESETUP_MEDIATYPE { const GUID *clsMajorType, *clsMinorType; };
struct FFMPEG_SUBTYPE_MAP { const GUID *clsMinorType; AVCodecID nFFCodec; };
const FFMPEG_SUBTYPE_MAP lavc_audio_codecs[] = {
    {&MEDIASUBTYPE_FLAC, AV_CODEC_ID_FLAC}, {&MEDIASUBTYPE_DOLBY_DDPLUS, AV_CODEC_ID_EAC3}};
struct Base
{
    HRESULT SetMediaType(PIN_DIRECTION, const CMediaType *) { ++commits; return S_OK; }
    unsigned commits = 0;
};
struct CLAVAudio : Base
{
    static const AMOVIESETUP_MEDIATYPE sudPinTypesIn[];
    static const UINT sudPinTypesInCount;
    struct { bool AllowRawSPDIF = false; bool pcmEnabled = true; } m_settings;
    struct Context { WORD bits_per_raw_sample = 0; } context;
    Context *m_pAVCtx = &context;
    bool m_bDVDPlayback = false;
    AVCodecID lastCodec = AV_CODEC_ID_NONE;
    unsigned initializations = 0;
    HRESULT ffmpeg_init(AVCodecID codec, const void *, GUID, DWORD)
    {
        if (!m_settings.pcmEnabled && codec >= AV_CODEC_ID_PCM_U8 && codec <= AV_CODEC_ID_PCM_F64LE)
            return VFW_E_UNSUPPORTED_AUDIO;
        ++initializations;
        lastCodec = codec;
        context.bits_per_raw_sample = 0;
        return S_OK;
    }
    HRESULT CheckInputType(const CMediaType *);
    HRESULT SetMediaType(PIN_DIRECTION, const CMediaType *);
};
#include "NormalPcmProductionMethods.inc"

struct Fixture
{
    WAVEFORMATEXTENSIBLE wave{};
    CMediaType type;
    Fixture(WORD bits = 24, bool floating = false, bool extensible = true,
            WORD valid = 0, WORD channels = 2, DWORD mask = 3)
    {
        wave.Format = {WORD(extensible ? WAVE_FORMAT_EXTENSIBLE : floating ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM),
            channels, 48000, DWORD(48000u * channels * (bits / 8)), WORD(channels * (bits / 8)), bits,
            WORD(extensible ? 22 : 0)};
        wave.Samples.wValidBitsPerSample = valid ? valid : bits;
        wave.dwChannelMask = mask;
        wave.SubFormat = floating ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
        type.subtype = wave.SubFormat;
        type.cbFormat = extensible ? sizeof(wave) : sizeof(wave.Format);
        type.pbFormat = reinterpret_cast<BYTE *>(&wave);
    }
};

void Rejected(Fixture &f)
{
    WORD valid = 123;
    assert(FindOpenJocNormalPcmCodec(&f.type, &valid) == AV_CODEC_ID_NONE && valid == 0);
#ifdef LAV_OPENJOC_SIDE_BY_SIDE
    for (bool allowRaw : {false, true})
    {
        CLAVAudio filter;
        filter.m_settings.AllowRawSPDIF = allowRaw;
        assert(filter.CheckInputType(&f.type) == VFW_E_TYPE_NOT_ACCEPTED);
        assert(filter.SetMediaType(PINDIR_INPUT, &f.type) == VFW_E_TYPE_NOT_ACCEPTED);
        assert(filter.initializations == 0 && filter.commits == 0);
    }
#endif
}

int main()
{
    const struct { WORD bits; bool floating; AVCodecID codec; } formats[] = {
        {8, false, AV_CODEC_ID_PCM_U8}, {16, false, AV_CODEC_ID_PCM_S16LE},
        {24, false, AV_CODEC_ID_PCM_S24LE}, {32, false, AV_CODEC_ID_PCM_S32LE},
        {32, true, AV_CODEC_ID_PCM_F32LE}, {64, true, AV_CODEC_ID_PCM_F64LE}};
    for (const auto &format : formats)
        for (bool extensible : {false, true})
        {
            Fixture f(format.bits, format.floating, extensible);
            WORD valid = 0;
            assert(FindOpenJocNormalPcmCodec(&f.type, &valid) == format.codec && valid == format.bits);
            CLAVAudio filter;
#ifdef LAV_OPENJOC_SIDE_BY_SIDE
            assert(filter.CheckInputType(&f.type) == S_OK);
            assert(FindCodecId(&f.type) == format.codec);
            assert(filter.SetMediaType(PINDIR_INPUT, &f.type) == S_OK);
            assert(filter.lastCodec == format.codec);
            assert(filter.context.bits_per_raw_sample == (format.floating ? 32 : format.bits));
            assert(filter.initializations == 1 && filter.commits == 1 && !filter.m_settings.AllowRawSPDIF);
            filter.m_settings.pcmEnabled = false;
            assert(filter.SetMediaType(PINDIR_INPUT, &f.type) == VFW_E_UNSUPPORTED_AUDIO);
#else
            // Non-side-by-side builds keep upstream opt-in behavior unchanged.
            assert(filter.CheckInputType(&f.type) == VFW_E_TYPE_NOT_ACCEPTED);
            assert(FindCodecId(&f.type) == AV_CODEC_ID_NONE);
            assert(filter.SetMediaType(PINDIR_INPUT, &f.type) == VFW_E_TYPE_NOT_ACCEPTED);
#endif
        }
    for (WORD precision : {WORD(16), WORD(20), WORD(24), WORD(32)})
    {
        Fixture f(32, false, true, precision);
        WORD valid = 0;
        assert(FindOpenJocNormalPcmCodec(&f.type, &valid) == AV_CODEC_ID_PCM_S32LE && valid == precision);
#ifdef LAV_OPENJOC_SIDE_BY_SIDE
        CLAVAudio filter;
        assert(filter.SetMediaType(PINDIR_INPUT, &f.type) == S_OK);
        assert(filter.lastCodec == AV_CODEC_ID_PCM_S32LE && filter.context.bits_per_raw_sample == precision);
#endif
    }
    for (DWORD mask : {DWORD(0), DWORD(0x3f), DWORD(0x60f)})
    {
        Fixture f(24, false, true, 24, 6, mask);
        assert(FindOpenJocNormalPcmCodec(&f.type) == AV_CODEC_ID_PCM_S24LE);
    }
    { Fixture f; f.type.pbFormat = nullptr; Rejected(f); }
    for (DWORD length = 0; length < sizeof(WAVEFORMATEXTENSIBLE); ++length)
    { Fixture f; f.type.cbFormat = length; Rejected(f); }
    { Fixture f; f.type.majortype = GUID{999}; Rejected(f); }
    { Fixture f; f.type.formattype = FORMAT_WaveFormatExFFMPEG; Rejected(f); }
    { Fixture f; f.wave.Format.cbSize = 21; Rejected(f); }
    { Fixture f; f.wave.Format.cbSize = 23; Rejected(f); }
    { Fixture f; f.wave.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; Rejected(f); }
    { Fixture f; f.wave.SubFormat = MEDIASUBTYPE_IEEE_FLOAT; Rejected(f); }
    { Fixture f; f.wave.Format.nChannels = 0; Rejected(f); }
    { Fixture f; f.wave.Format.nChannels = 65; Rejected(f); }
    { Fixture f; f.wave.Format.nSamplesPerSec = 0; Rejected(f); }
    { Fixture f; f.wave.Format.nSamplesPerSec = 0xffffffffu; Rejected(f); }
    { Fixture f(8, false, true, 8, 1, 4); f.wave.Format.nSamplesPerSec = 0x80000000u;
      f.wave.Format.nAvgBytesPerSec = 0x80000000u; Rejected(f); }
    { Fixture f; f.wave.Format.nAvgBytesPerSec = 0; Rejected(f); }
    { Fixture f; f.wave.Format.nBlockAlign = 8; Rejected(f); }
    { Fixture f(32); f.wave.Format.nBlockAlign = 6; Rejected(f); }
    { Fixture f; f.wave.Samples.wValidBitsPerSample = 0; Rejected(f); }
    { Fixture f; f.wave.Samples.wValidBitsPerSample = 25; Rejected(f); }
    { Fixture f(32, true); f.wave.Samples.wValidBitsPerSample = 24; Rejected(f); }
    { Fixture f; f.wave.dwChannelMask = 1; Rejected(f); }
    { Fixture f; f.wave.dwChannelMask = 7; Rejected(f); }
    { Fixture f; f.wave.dwChannelMask = 0xc0000000u; Rejected(f); }
    for (WORD bits : {WORD(0), WORD(12), WORD(20), WORD(48), WORD(64)})
    { Fixture f(bits, false); Rejected(f); }
    for (WORD bits : {WORD(8), WORD(16), WORD(24), WORD(48)})
    { Fixture f(bits, true); Rejected(f); }
    // A deliberately unaligned format buffer is safe to validate.
    { Fixture f; std::vector<BYTE> data(sizeof(f.wave) + 1); std::memcpy(data.data() + 1, &f.wave, sizeof(f.wave));
      f.type.pbFormat = data.data() + 1; assert(FindOpenJocNormalPcmCodec(&f.type) == AV_CODEC_ID_PCM_S24LE); }
    for (const auto subtype : {MEDIASUBTYPE_FLAC, MEDIASUBTYPE_DOLBY_DDPLUS})
    { Fixture f; f.type.subtype = subtype; CLAVAudio filter;
      assert(filter.CheckInputType(&f.type) == S_OK && filter.SetMediaType(PINDIR_INPUT, &f.type) == S_OK); }
    { Fixture f(16); f.type.subtype = MEDIASUBTYPE_DOLBY_AC3_SPDIF; CLAVAudio filter;
      assert(filter.CheckInputType(&f.type) == VFW_E_TYPE_NOT_ACCEPTED);
      assert(filter.SetMediaType(PINDIR_INPUT, &f.type) == VFW_E_TYPE_NOT_ACCEPTED);
      filter.m_settings.AllowRawSPDIF = true;
      assert(filter.CheckInputType(&f.type) == S_OK && filter.SetMediaType(PINDIR_INPUT, &f.type) == S_OK);
      assert(filter.lastCodec == AV_CODEC_ID_AC3); }
#ifdef LAV_OPENJOC_SIDE_BY_SIDE
    assert(CLAVAudio::sudPinTypesInCount == 4);
    std::cout << "Normal PCM side-by-side admission, mapping and initialization tests passed\n";
#else
    assert(CLAVAudio::sudPinTypesInCount == 2);
    std::cout << "Stock build PCM admission behavior preserved\n";
#endif
}
