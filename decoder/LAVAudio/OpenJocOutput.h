/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "LAVOpenJocSettings.h"

#include <libavutil/channel_layout.h>

#include <cstddef>
#include <cstdint>

inline constexpr std::size_t LAV_OPENJOC_OUTPUT_CONTRACT_COUNT = 7;

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
};

[[nodiscard]] const LAVOpenJocOutputContract *
FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy policy) noexcept;
