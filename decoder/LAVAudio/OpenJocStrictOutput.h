/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "OpenJocOutput.h"

#include <windows.h>
#include <dshow.h>
#include <vfwmsgs.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstddef>
#include <cstdint>

struct LAVOpenJocStrictMediaType
{
    GUID major_type{};
    GUID subtype{};
    BOOL fixed_size_samples = FALSE;
    BOOL temporal_compression = FALSE;
    GUID format_type{};
    ULONG format_size = 0;
    WAVEFORMATEXTENSIBLE wave{};
    ULONG sample_size = 0;
};

[[nodiscard]] bool BuildLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,
                                                  LAVOpenJocStrictMediaType *output) noexcept;

[[nodiscard]] bool IsExactLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,
                                                    const LAVOpenJocStrictMediaType &candidate) noexcept;

[[nodiscard]] bool IsExactLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,
                                                    const AM_MEDIA_TYPE &candidate) noexcept;

[[nodiscard]] bool CheckedLAVOpenJocSampleAdd(std::uint32_t left, std::uint32_t right,
                                              std::uint32_t *output) noexcept;

[[nodiscard]] bool CheckedLAVOpenJocPcmByteCount(std::size_t sample_count, std::uint16_t block_align,
                                                std::size_t *output) noexcept;

[[nodiscard]] bool CheckedLAVOpenJocLongNarrow(std::size_t value, long *output) noexcept;

[[nodiscard]] bool CheckedLAVOpenJocAllocatorGrowth(long required_bytes, long *output) noexcept;

[[nodiscard]] bool AreLAVOpenJocBufferContractsCompatible(const LAVOpenJocOutputContract *queued,
                                                          const LAVOpenJocOutputContract *incoming) noexcept;

[[nodiscard]] bool ValidateLAVOpenJocStrictBuffer(const LAVOpenJocOutputContract *contract, bool is_float32,
                                                 std::uint32_t sample_rate, bool is_planar,
                                                 const AVChannelLayout &layout, std::uint32_t sample_count,
                                                 std::size_t buffer_bytes) noexcept;

[[nodiscard]] HRESULT NormalizeLAVOpenJocQueryAcceptResult(HRESULT result) noexcept;

[[nodiscard]] HRESULT NormalizeLAVOpenJocEndOfStreamStep(bool strict_openjoc, HRESULT result) noexcept;

[[nodiscard]] HRESULT ValidateLAVOpenJocDeliverySample(const LAVOpenJocOutputContract &contract,
                                                       const AM_MEDIA_TYPE *attached_type, long capacity,
                                                       long required_bytes) noexcept;
