/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core
// Included after the DirectShow, WAVEFORMATEX and AVCodecID declarations.
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

inline bool IsOpenJocNormalPcmSubtype(const CMediaType *type)
{
    return type && (type->subtype == MEDIASUBTYPE_PCM || type->subtype == MEDIASUBTYPE_IEEE_FLOAT);
}

// Validate before either admission or codec initialization. The codec is chosen
// by storage width, never valid precision: 24 valid bits in a 32-bit container
// is S32LE, while packed 24-bit samples are S24LE. Extensible integer samples
// are left-aligned, as required by WAVEFORMATEXTENSIBLE.
inline AVCodecID FindOpenJocNormalPcmCodec(const CMediaType *type, WORD *valid_bits = nullptr)
{
    if (valid_bits)
        *valid_bits = 0;
    if (!IsOpenJocNormalPcmSubtype(type) || type->majortype != MEDIATYPE_Audio ||
        type->formattype != FORMAT_WaveFormatEx || !type->pbFormat || type->cbFormat < sizeof(WAVEFORMATEX))
        return AV_CODEC_ID_NONE;

    WAVEFORMATEX wave = {};
    std::memcpy(&wave, type->pbFormat, sizeof(wave));
    if (wave.cbSize > type->cbFormat - sizeof(WAVEFORMATEX) || wave.nChannels == 0 || wave.nChannels > 64 ||
        wave.nSamplesPerSec == 0 || wave.nSamplesPerSec > (std::numeric_limits<std::int32_t>::max)())
        return AV_CODEC_ID_NONE;

    const bool floating = type->subtype == MEDIASUBTYPE_IEEE_FLOAT;
    WORD precision = wave.wBitsPerSample;
    if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
            type->cbFormat < sizeof(WAVEFORMATEXTENSIBLE))
            return AV_CODEC_ID_NONE;
        WAVEFORMATEXTENSIBLE extended = {};
        std::memcpy(&extended, type->pbFormat, sizeof(extended));
        if (extended.SubFormat != type->subtype)
            return AV_CODEC_ID_NONE;
        precision = extended.Samples.wValidBitsPerSample;
        if (precision == 0 || precision > wave.wBitsPerSample || (floating && precision != wave.wBitsPerSample))
            return AV_CODEC_ID_NONE;
        // Zero means unspecified/direct-out. Otherwise require one standard
        // Windows speaker position per channel; do not silently replace a mask.
        DWORD mask = extended.dwChannelMask;
        if (mask & ~DWORD(0x0003ffff))
            return AV_CODEC_ID_NONE;
        unsigned speakers = 0;
        for (DWORD remaining = mask; remaining; remaining &= remaining - 1)
            ++speakers;
        if (mask && speakers != wave.nChannels)
            return AV_CODEC_ID_NONE;
    }
    else if (wave.wFormatTag != (floating ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM))
        return AV_CODEC_ID_NONE;

    AVCodecID codec = AV_CODEC_ID_NONE;
    if (floating)
    {
        if (wave.wBitsPerSample == 32)
            codec = AV_CODEC_ID_PCM_F32LE;
        else if (wave.wBitsPerSample == 64)
            codec = AV_CODEC_ID_PCM_F64LE;
    }
    else
    {
        switch (wave.wBitsPerSample)
        {
        case 8: codec = AV_CODEC_ID_PCM_U8; break;
        case 16: codec = AV_CODEC_ID_PCM_S16LE; break;
        case 24: codec = AV_CODEC_ID_PCM_S24LE; break;
        case 32: codec = AV_CODEC_ID_PCM_S32LE; break;
        }
    }
    if (codec == AV_CODEC_ID_NONE)
        return AV_CODEC_ID_NONE;

    const unsigned block_align = unsigned(wave.nChannels) * (wave.wBitsPerSample / 8);
    const std::uint64_t bytes_per_second = std::uint64_t(wave.nSamplesPerSec) * block_align;
    if (wave.nBlockAlign != block_align || bytes_per_second != wave.nAvgBytesPerSec)
        return AV_CODEC_ID_NONE;
    if (valid_bits)
        *valid_bits = precision;
    return codec;
}
