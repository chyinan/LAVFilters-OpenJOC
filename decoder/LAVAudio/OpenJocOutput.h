/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "LAVOpenJocSettings.h"

extern "C"
{
#include <libavutil/channel_layout.h>
}

#include <cstddef>
#include <cstdint>

inline constexpr std::size_t LAV_OPENJOC_OUTPUT_CONTRACT_COUNT = 8;
inline constexpr std::uint32_t LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32 = 1;

struct LAVOpenJocOutputContract
{
    LAVOpenJocOutputPolicy policy;
    const char *property_page_label;
    const char *abi_preset_name;
    const char *openjoc_layout_name;
    const char *ffmpeg_standard_layout_name;
    const char *const *openjoc_semantic_labels;
    const AVChannel *ordered_channels;
    std::uint32_t channel_count;
    std::uint64_t ffmpeg_channel_mask;
    std::uint32_t windows_channel_mask;
    const char *const *frame_channel_labels;
    const char *openjoc_frame_layout_name;
};

[[nodiscard]] const LAVOpenJocOutputContract *
FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy policy) noexcept;

[[nodiscard]] bool BuildOpenJocAvChannelLayout(const LAVOpenJocOutputContract &contract,
                                               AVChannelLayout *output) noexcept;

[[nodiscard]] bool ValidateLAVOpenJocFrameMetadata(
    const LAVOpenJocOutputContract &contract, std::uint32_t sample_format, std::uint32_t sample_rate,
    std::uint32_t channel_count, std::size_t sample_count, std::size_t data_len, const char *layout_name,
    const char *const *channel_labels, std::size_t channel_label_count, std::size_t *validated_element_count,
    std::size_t *validated_byte_count) noexcept;

[[nodiscard]] bool PrepareLAVOpenJocFrameHandoff(
    const LAVOpenJocOutputContract *current_contract, const LAVOpenJocOutputContract *frame_contract,
    std::uint32_t sample_rate, std::uint32_t channel_count, std::size_t sample_count,
    std::size_t sample_element_count, AVChannelLayout *output_layout, std::uint32_t *output_sample_count,
    std::uint32_t *output_byte_count) noexcept;
