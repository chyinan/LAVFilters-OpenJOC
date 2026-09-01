/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocOutput.h"

#include <array>
#include <cstring>
#include <limits>

namespace
{
constexpr const char *kStereoLabels[] = {"FL", "FR"};
constexpr AVChannel kStereoChannels[] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT};
constexpr const char *kBinauralLabels[] = {"Left Ear", "Right Ear"};
constexpr const char *kBinauralFrameLabels[] = {"BIL", "BIR"};

constexpr const char *kLayout51Labels[] = {"FL", "FR", "FC", "LFE", "Ls", "Rs"};
constexpr AVChannel kLayout51Channels[] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
                                           AV_CHAN_LOW_FREQUENCY, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT};

constexpr const char *kLayout71Labels[] = {"FL", "FR", "FC", "LFE", "Lb", "Rb", "Ls", "Rs"};
constexpr AVChannel kLayout71Channels[] = {AV_CHAN_FRONT_LEFT,  AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
                                           AV_CHAN_LOW_FREQUENCY, AV_CHAN_BACK_LEFT,   AV_CHAN_BACK_RIGHT,
                                           AV_CHAN_SIDE_LEFT,   AV_CHAN_SIDE_RIGHT};

constexpr const char *kLayout512Labels[] = {"FL", "FR", "FC", "LFE", "Ls", "Rs", "TFL", "TFR"};
constexpr AVChannel kLayout512Channels[] = {
    AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,  AV_CHAN_LOW_FREQUENCY,
    AV_CHAN_SIDE_LEFT,  AV_CHAN_SIDE_RIGHT,  AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT};

constexpr const char *kLayout514Labels[] = {"FL", "FR", "FC",  "LFE", "Ls",
                                            "Rs", "TFL", "TFR", "TBL", "TBR"};
constexpr AVChannel kLayout514Channels[] = {
    AV_CHAN_FRONT_LEFT,     AV_CHAN_FRONT_RIGHT,     AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
    AV_CHAN_SIDE_LEFT,      AV_CHAN_SIDE_RIGHT,      AV_CHAN_TOP_FRONT_LEFT,
    AV_CHAN_TOP_FRONT_RIGHT, AV_CHAN_TOP_BACK_LEFT,  AV_CHAN_TOP_BACK_RIGHT};

constexpr const char *kLayout712Labels[] = {"FL", "FR", "FC", "LFE", "Lb",
                                            "Rb", "Ls", "Rs", "TFL", "TFR"};
constexpr AVChannel kLayout712Channels[] = {
    AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
    AV_CHAN_BACK_LEFT,  AV_CHAN_BACK_RIGHT,  AV_CHAN_SIDE_LEFT,    AV_CHAN_SIDE_RIGHT,
    AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT};

constexpr const char *kLayout714Labels[] = {"FL", "FR", "FC",  "LFE", "Lb",  "Rb",
                                            "Ls", "Rs", "TFL", "TFR", "TBL", "TBR"};
constexpr AVChannel kLayout714Channels[] = {
    AV_CHAN_FRONT_LEFT,      AV_CHAN_FRONT_RIGHT,     AV_CHAN_FRONT_CENTER,
    AV_CHAN_LOW_FREQUENCY,   AV_CHAN_BACK_LEFT,       AV_CHAN_BACK_RIGHT,
    AV_CHAN_SIDE_LEFT,       AV_CHAN_SIDE_RIGHT,      AV_CHAN_TOP_FRONT_LEFT,
    AV_CHAN_TOP_FRONT_RIGHT, AV_CHAN_TOP_BACK_LEFT,   AV_CHAN_TOP_BACK_RIGHT};

constexpr std::array<LAVOpenJocOutputContract, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> kContracts = {{
    {LAVOpenJocOutputPolicy::Stereo, "Stereo", nullptr, "2.0", "stereo", kStereoLabels,
     kStereoChannels, 2, 0x00000003u, 0x00000003u},
    {LAVOpenJocOutputPolicy::Layout51, "5.1", "5.1", "5.1", "5.1(side)", kLayout51Labels,
     kLayout51Channels, 6, 0x0000060fu, 0x0000060fu},
    {LAVOpenJocOutputPolicy::Layout71, "7.1", "7.1", "7.1", "7.1", kLayout71Labels,
     kLayout71Channels, 8, 0x0000063fu, 0x0000063fu},
    {LAVOpenJocOutputPolicy::Layout512, "5.1.2", "5.1.2", "5.1.2", "5.1.2", kLayout512Labels,
     kLayout512Channels, 8, 0x0000560fu, 0x0000560fu},
    {LAVOpenJocOutputPolicy::Layout514, "5.1.4", "5.1.4", "5.1.4", "5.1.4", kLayout514Labels,
     kLayout514Channels, 10, 0x0002d60fu, 0x0002d60fu},
    {LAVOpenJocOutputPolicy::Layout712, "7.1.2", "7.1.2", "7.1.2", "7.1.2", kLayout712Labels,
     kLayout712Channels, 10, 0x0000563fu, 0x0000563fu},
    {LAVOpenJocOutputPolicy::Layout714, "7.1.4", "7.1.4", "7.1.4", "7.1.4", kLayout714Labels,
     kLayout714Channels, 12, 0x0002d63fu, 0x0002d63fu},
    {LAVOpenJocOutputPolicy::Binaural, "Binaural (Headphones)", "7.1.4", "binaural", "stereo",
     kBinauralLabels, kStereoChannels, 2, 0x00000003u, 0x00000003u, kBinauralFrameLabels, "binaural"},
}};

constexpr std::uint64_t kMappedWindowsSpeakerBits = 0x0003ffffu;

std::uint32_t CountBits(std::uint64_t value) noexcept
{
    std::uint32_t count = 0;
    while (value != 0)
    {
        count += static_cast<std::uint32_t>(value & 1u);
        value >>= 1;
    }
    return count;
}

AVChannel SemanticLabelToAvChannel(const char *label) noexcept
{
    if (!label)
        return AV_CHAN_NONE;
    if (std::strcmp(label, "FL") == 0)
        return AV_CHAN_FRONT_LEFT;
    if (std::strcmp(label, "FR") == 0)
        return AV_CHAN_FRONT_RIGHT;
    if (std::strcmp(label, "FC") == 0)
        return AV_CHAN_FRONT_CENTER;
    if (std::strcmp(label, "LFE") == 0)
        return AV_CHAN_LOW_FREQUENCY;
    if (std::strcmp(label, "Lb") == 0)
        return AV_CHAN_BACK_LEFT;
    if (std::strcmp(label, "Rb") == 0)
        return AV_CHAN_BACK_RIGHT;
    if (std::strcmp(label, "Ls") == 0)
        return AV_CHAN_SIDE_LEFT;
    if (std::strcmp(label, "Rs") == 0)
        return AV_CHAN_SIDE_RIGHT;
    if (std::strcmp(label, "TFL") == 0)
        return AV_CHAN_TOP_FRONT_LEFT;
    if (std::strcmp(label, "TFR") == 0)
        return AV_CHAN_TOP_FRONT_RIGHT;
    if (std::strcmp(label, "TBL") == 0)
        return AV_CHAN_TOP_BACK_LEFT;
    if (std::strcmp(label, "TBR") == 0)
        return AV_CHAN_TOP_BACK_RIGHT;
    if (std::strcmp(label, "Left Ear") == 0)
        return AV_CHAN_FRONT_LEFT;
    if (std::strcmp(label, "Right Ear") == 0)
        return AV_CHAN_FRONT_RIGHT;
    return AV_CHAN_NONE;
}

const char *AvChannelToFfmpegLabel(const AVChannel channel) noexcept
{
    switch (channel)
    {
    case AV_CHAN_FRONT_LEFT: return "FL";
    case AV_CHAN_FRONT_RIGHT: return "FR";
    case AV_CHAN_FRONT_CENTER: return "FC";
    case AV_CHAN_LOW_FREQUENCY: return "LFE";
    case AV_CHAN_BACK_LEFT: return "BL";
    case AV_CHAN_BACK_RIGHT: return "BR";
    case AV_CHAN_SIDE_LEFT: return "SL";
    case AV_CHAN_SIDE_RIGHT: return "SR";
    case AV_CHAN_TOP_FRONT_LEFT: return "TFL";
    case AV_CHAN_TOP_FRONT_RIGHT: return "TFR";
    case AV_CHAN_TOP_BACK_LEFT: return "TBL";
    case AV_CHAN_TOP_BACK_RIGHT: return "TBR";
    default: return nullptr;
    }
}

const char *FrameLabelForContract(const LAVOpenJocOutputContract &contract,
                                  const std::uint32_t index) noexcept
{
    if (contract.frame_channel_labels)
        return contract.frame_channel_labels[index];
    return AvChannelToFfmpegLabel(contract.ordered_channels[index]);
}

bool IsCanonicalContractShape(const LAVOpenJocOutputContract &contract) noexcept
{
    if (!contract.openjoc_layout_name || !contract.openjoc_semantic_labels || !contract.ordered_channels ||
        contract.channel_count == 0 || contract.channel_count > 12 || contract.ffmpeg_channel_mask == 0 ||
        contract.ffmpeg_channel_mask != contract.windows_channel_mask ||
        (contract.ffmpeg_channel_mask & ~kMappedWindowsSpeakerBits) != 0 ||
        CountBits(contract.ffmpeg_channel_mask) != contract.channel_count)
    {
        return false;
    }

    std::uint32_t channel_index = 0;
    for (std::uint32_t bit = 0; bit < 64; ++bit)
    {
        if ((contract.ffmpeg_channel_mask & (std::uint64_t{1} << bit)) == 0)
            continue;
        if (channel_index >= contract.channel_count ||
            contract.ordered_channels[channel_index] != static_cast<AVChannel>(bit) ||
            SemanticLabelToAvChannel(contract.openjoc_semantic_labels[channel_index]) !=
                contract.ordered_channels[channel_index])
        {
            return false;
        }
        ++channel_index;
    }
    return channel_index == contract.channel_count;
}
} // namespace

const LAVOpenJocOutputContract *FindLAVOpenJocOutputContract(const LAVOpenJocOutputPolicy policy) noexcept
{
    const std::uint32_t wire_value = static_cast<std::uint32_t>(policy);
    if (wire_value >= kContracts.size())
        return nullptr;

    const LAVOpenJocOutputContract &contract = kContracts[wire_value];
    return contract.policy == policy ? &contract : nullptr;
}

bool BuildOpenJocAvChannelLayout(const LAVOpenJocOutputContract &contract, AVChannelLayout *output) noexcept
{
    if (!output)
        return false;

    av_channel_layout_uninit(output);
    if (!IsCanonicalContractShape(contract))
        return false;

    AVChannelLayout candidate{};
    if (av_channel_layout_from_mask(&candidate, contract.ffmpeg_channel_mask) < 0 ||
        av_channel_layout_check(&candidate) != 1 || candidate.order != AV_CHANNEL_ORDER_NATIVE ||
        candidate.nb_channels != static_cast<int>(contract.channel_count) ||
        candidate.u.mask != contract.ffmpeg_channel_mask)
    {
        av_channel_layout_uninit(&candidate);
        return false;
    }

    for (std::uint32_t index = 0; index < contract.channel_count; ++index)
    {
        if (av_channel_layout_channel_from_index(&candidate, index) != contract.ordered_channels[index])
        {
            av_channel_layout_uninit(&candidate);
            return false;
        }
    }

    const int copy_status = av_channel_layout_copy(output, &candidate);
    av_channel_layout_uninit(&candidate);
    if (copy_status != 0 || av_channel_layout_check(output) != 1)
    {
        av_channel_layout_uninit(output);
        return false;
    }
    return true;
}

bool ValidateLAVOpenJocFrameMetadata(
    const LAVOpenJocOutputContract &contract, const std::uint32_t sample_format,
    const std::uint32_t sample_rate, const std::uint32_t channel_count, const std::size_t sample_count,
    const std::size_t data_len, const char *layout_name, const char *const *channel_labels,
    const std::size_t channel_label_count, std::size_t *validated_element_count,
    std::size_t *validated_byte_count) noexcept
{
    if (!IsCanonicalContractShape(contract) || !contract.ffmpeg_standard_layout_name ||
        sample_format != LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32 ||
        sample_rate != 48000 || channel_count != contract.channel_count || sample_count == 0 || !layout_name ||
        std::strcmp(layout_name, contract.openjoc_frame_layout_name
                                  ? contract.openjoc_frame_layout_name
                                  : contract.ffmpeg_standard_layout_name) != 0 ||
        !channel_labels ||
        channel_label_count != contract.channel_count)
    {
        return false;
    }

    for (std::uint32_t index = 0; index < contract.channel_count; ++index)
    {
        const char *label = channel_labels[index];
        const char *expected_label = FrameLabelForContract(contract, index);
        if (!label || !expected_label || std::strcmp(label, expected_label) != 0)
        {
            return false;
        }
    }

    if (sample_count > (std::numeric_limits<std::size_t>::max)() / channel_count)
        return false;
    const std::size_t element_count = sample_count * channel_count;
    if (data_len != element_count || element_count > (std::numeric_limits<std::size_t>::max)() / sizeof(float))
        return false;
    const std::size_t byte_count = element_count * sizeof(float);

    if (validated_element_count)
        *validated_element_count = element_count;
    if (validated_byte_count)
        *validated_byte_count = byte_count;
    return true;
}

bool PrepareLAVOpenJocFrameHandoff(
    const LAVOpenJocOutputContract *current_contract, const LAVOpenJocOutputContract *frame_contract,
    const std::uint32_t sample_rate, const std::uint32_t channel_count, const std::size_t sample_count,
    const std::size_t sample_element_count, AVChannelLayout *output_layout,
    std::uint32_t *output_sample_count, std::uint32_t *output_byte_count) noexcept
{
    if (!output_layout || !output_sample_count || !output_byte_count)
        return false;

    av_channel_layout_uninit(output_layout);
    if (!current_contract || frame_contract != current_contract || sample_rate != 48000 ||
        channel_count != current_contract->channel_count || sample_count == 0 ||
        sample_count > (std::numeric_limits<std::uint32_t>::max)() || channel_count == 0 ||
        sample_count > (std::numeric_limits<std::size_t>::max)() / channel_count)
    {
        return false;
    }

    const std::size_t element_count = sample_count * channel_count;
    if (sample_element_count != element_count ||
        element_count > (std::numeric_limits<std::uint32_t>::max)() / sizeof(float) ||
        !BuildOpenJocAvChannelLayout(*current_contract, output_layout))
    {
        return false;
    }

    *output_sample_count = static_cast<std::uint32_t>(sample_count);
    *output_byte_count = static_cast<std::uint32_t>(element_count * sizeof(float));
    return true;
}
