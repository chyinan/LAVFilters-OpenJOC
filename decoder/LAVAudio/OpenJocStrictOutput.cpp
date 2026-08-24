/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocStrictOutput.h"

#include <cstring>
#include <limits>

namespace
{
bool IsCanonicalContract(const LAVOpenJocOutputContract &contract) noexcept
{
    return FindLAVOpenJocOutputContract(contract.policy) == &contract && contract.channel_count > 0 &&
           contract.channel_count <= (std::numeric_limits<WORD>::max)() && contract.windows_channel_mask != 0 &&
           contract.windows_channel_mask == contract.ffmpeg_channel_mask &&
           __popcnt(contract.windows_channel_mask) == contract.channel_count;
}
} // namespace

bool BuildLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,
                                    LAVOpenJocStrictMediaType *output) noexcept
{
    if (!output)
        return false;
    *output = {};
    if (!IsCanonicalContract(contract))
        return false;

    const std::uint64_t block_align = static_cast<std::uint64_t>(contract.channel_count) * sizeof(float);
    const std::uint64_t average_bytes = block_align * 48000u;
    if (block_align > (std::numeric_limits<WORD>::max)() ||
        average_bytes > (std::numeric_limits<DWORD>::max)())
    {
        return false;
    }

    WAVEFORMATEXTENSIBLE &wave = output->wave;
    output->major_type = MEDIATYPE_Audio;
    output->subtype = MEDIASUBTYPE_IEEE_FLOAT;
    output->fixed_size_samples = TRUE;
    output->temporal_compression = FALSE;
    output->format_type = FORMAT_WaveFormatEx;
    output->format_size = sizeof(WAVEFORMATEXTENSIBLE);
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(contract.channel_count);
    wave.Format.nSamplesPerSec = 48000;
    wave.Format.nAvgBytesPerSec = static_cast<DWORD>(average_bytes);
    wave.Format.nBlockAlign = static_cast<WORD>(block_align);
    wave.Format.wBitsPerSample = 32;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = 32;
    wave.dwChannelMask = contract.windows_channel_mask;
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    output->sample_size = wave.Format.nBlockAlign;
    return true;
}

bool IsExactLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,
                                      const LAVOpenJocStrictMediaType &candidate) noexcept
{
    LAVOpenJocStrictMediaType expected{};
    return BuildLAVOpenJocStrictMediaType(contract, &expected) &&
           std::memcmp(&candidate, &expected, sizeof(expected)) == 0;
}

bool IsExactLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,
                                      const AM_MEDIA_TYPE &candidate) noexcept
{
    LAVOpenJocStrictMediaType expected{};
    return BuildLAVOpenJocStrictMediaType(contract, &expected) && candidate.majortype == expected.major_type &&
           candidate.subtype == expected.subtype && candidate.bFixedSizeSamples == expected.fixed_size_samples &&
           candidate.bTemporalCompression == expected.temporal_compression &&
           candidate.lSampleSize == expected.sample_size && candidate.formattype == expected.format_type &&
           candidate.pUnk == nullptr && candidate.cbFormat == expected.format_size && candidate.pbFormat != nullptr &&
           std::memcmp(candidate.pbFormat, &expected.wave, expected.format_size) == 0;
}

bool CheckedLAVOpenJocSampleAdd(const std::uint32_t left, const std::uint32_t right,
                                std::uint32_t *output) noexcept
{
    if (!output)
        return false;
    *output = 0;
    if (right > (std::numeric_limits<std::uint32_t>::max)() - left)
        return false;
    *output = left + right;
    return true;
}

bool CheckedLAVOpenJocPcmByteCount(const std::size_t sample_count, const std::uint16_t block_align,
                                   std::size_t *output) noexcept
{
    if (!output)
        return false;
    *output = 0;
    if (block_align == 0 || sample_count > (std::numeric_limits<std::size_t>::max)() / block_align)
        return false;
    *output = sample_count * block_align;
    return true;
}

bool CheckedLAVOpenJocLongNarrow(const std::size_t value, long *output) noexcept
{
    if (!output)
        return false;
    *output = 0;
    if (value > static_cast<std::size_t>((std::numeric_limits<long>::max)()))
        return false;
    *output = static_cast<long>(value);
    return true;
}

bool CheckedLAVOpenJocAllocatorGrowth(const long required_bytes, long *output) noexcept
{
    if (!output)
        return false;
    *output = 0;
    if (required_bytes < 0 || required_bytes > (std::numeric_limits<long>::max)() - required_bytes / 2)
        return false;
    *output = required_bytes + required_bytes / 2;
    return true;
}

bool AreLAVOpenJocBufferContractsCompatible(const LAVOpenJocOutputContract *queued,
                                             const LAVOpenJocOutputContract *incoming) noexcept
{
    return queued == incoming;
}

bool ValidateLAVOpenJocStrictBuffer(const LAVOpenJocOutputContract *contract, const bool is_float32,
                                    const std::uint32_t sample_rate, const bool is_planar,
                                    const AVChannelLayout &layout, const std::uint32_t sample_count,
                                    const std::size_t buffer_bytes) noexcept
{
    if (!contract || FindLAVOpenJocOutputContract(contract->policy) != contract || !is_float32 ||
        sample_rate != 48000 || is_planar || sample_count == 0 || layout.order != AV_CHANNEL_ORDER_NATIVE ||
        layout.nb_channels != static_cast<int>(contract->channel_count) || layout.u.mask != contract->ffmpeg_channel_mask ||
        av_channel_layout_check(&layout) != 1)
    {
        return false;
    }

    std::size_t expected_bytes = 0;
    return CheckedLAVOpenJocPcmByteCount(sample_count,
                                        static_cast<std::uint16_t>(contract->channel_count * sizeof(float)),
                                        &expected_bytes) &&
           buffer_bytes == expected_bytes;
}

HRESULT NormalizeLAVOpenJocQueryAcceptResult(const HRESULT result) noexcept
{
    if (result == S_OK)
        return S_OK;
    return FAILED(result) ? result : VFW_E_TYPE_NOT_ACCEPTED;
}

HRESULT NormalizeLAVOpenJocEndOfStreamStep(const bool strict_openjoc, const HRESULT result) noexcept
{
    return strict_openjoc && FAILED(result) ? result : S_OK;
}

HRESULT ValidateLAVOpenJocDeliverySample(const LAVOpenJocOutputContract &contract,
                                         const AM_MEDIA_TYPE *attached_type, const long capacity,
                                         const long required_bytes) noexcept
{
    LAVOpenJocStrictMediaType expected{};
    if (!BuildLAVOpenJocStrictMediaType(contract, &expected) || required_bytes < 0 || capacity < 0)
        return E_INVALIDARG;
    if (attached_type && !IsExactLAVOpenJocStrictMediaType(contract, *attached_type))
        return VFW_E_TYPE_NOT_ACCEPTED;
    if (capacity < required_bytes)
        return VFW_E_BUFFER_UNDERFLOW;
    return S_OK;
}
