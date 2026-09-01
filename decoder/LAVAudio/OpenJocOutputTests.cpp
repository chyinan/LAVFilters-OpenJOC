/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Canonical OpenJOC output-policy contract tests.

// pattern: Functional Core

#include "LAVOpenJocSettings.h"
#include "OpenJocOutput.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>

namespace
{
constexpr std::size_t kMaximumChannels = 12;

template <typename Type, typename = void>
struct HasAutoPolicy : std::false_type
{};

template <typename Type>
struct HasAutoPolicy<Type, std::void_t<decltype(Type::Auto)>> : std::true_type
{};

template <typename Type, typename = void>
struct HasLayout524Policy : std::false_type
{};

template <typename Type>
struct HasLayout524Policy<Type, std::void_t<decltype(Type::Layout524)>> : std::true_type
{};

template <typename Type, typename = void>
struct HasLayout716Policy : std::false_type
{};

template <typename Type>
struct HasLayout716Policy<Type, std::void_t<decltype(Type::Layout716)>> : std::true_type
{};

template <typename Type, typename = void>
struct HasLayout91Policy : std::false_type
{};

template <typename Type>
struct HasLayout91Policy<Type, std::void_t<decltype(Type::Layout91)>> : std::true_type
{};

template <typename Type, typename = void>
struct HasLayout222Policy : std::false_type
{};

template <typename Type>
struct HasLayout222Policy<Type, std::void_t<decltype(Type::Layout222)>> : std::true_type
{};

static_assert(LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION == 1);
static_assert(sizeof(LAVOpenJocOutputPolicy) == sizeof(std::uint32_t));
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Stereo) == 0);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout51) == 1);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout71) == 2);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout512) == 3);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout514) == 4);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout712) == 5);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout714) == 6);
static_assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Binaural) == 7);
static_assert(LAV_OPENJOC_OUTPUT_CONTRACT_COUNT == 8);
static_assert(!HasAutoPolicy<LAVOpenJocOutputPolicy>::value);
static_assert(!HasLayout524Policy<LAVOpenJocOutputPolicy>::value);
static_assert(!HasLayout716Policy<LAVOpenJocOutputPolicy>::value);
static_assert(!HasLayout91Policy<LAVOpenJocOutputPolicy>::value);
static_assert(!HasLayout222Policy<LAVOpenJocOutputPolicy>::value);

using ContractLookup = decltype(&FindLAVOpenJocOutputContract);
static_assert(std::is_invocable_r_v<const LAVOpenJocOutputContract *, ContractLookup,
                                    LAVOpenJocOutputPolicy>);
static_assert(!std::is_invocable_v<ContractLookup, const char *>);
static_assert(!std::is_invocable_v<ContractLookup, std::uint32_t>);
static_assert(!std::is_invocable_v<ContractLookup, std::size_t>);

struct ExpectedContract
{
    LAVOpenJocOutputPolicy policy;
    const char *property_page_label;
    const char *abi_preset_name;
    const char *openjoc_layout_name;
    const char *ffmpeg_standard_layout_name;
    std::array<const char *, kMaximumChannels> semantic_labels;
    std::array<AVChannel, kMaximumChannels> ordered_channels;
    std::uint32_t channel_count;
    std::uint64_t mask;
};

const std::array<ExpectedContract, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT> kExpectedContracts = {{
    {LAVOpenJocOutputPolicy::Stereo,
     "Stereo (Speakers)",
     nullptr,
     "2.0",
     "stereo",
     {"FL", "FR"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT},
     2,
     0x00000003u},
    {LAVOpenJocOutputPolicy::Layout51,
     "5.1",
     "5.1",
     "5.1",
     "5.1(side)",
     {"FL", "FR", "FC", "LFE", "Ls", "Rs"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
      AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT},
     6,
     0x0000060fu},
    {LAVOpenJocOutputPolicy::Layout71,
     "7.1",
     "7.1",
     "7.1",
     "7.1",
     {"FL", "FR", "FC", "LFE", "Lb", "Rb", "Ls", "Rs"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
      AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT},
     8,
     0x0000063fu},
    {LAVOpenJocOutputPolicy::Layout512,
     "5.1.2",
     "5.1.2",
     "5.1.2",
     "5.1.2",
     {"FL", "FR", "FC", "LFE", "Ls", "Rs", "TFL", "TFR"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
      AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT, AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT},
     8,
     0x0000560fu},
    {LAVOpenJocOutputPolicy::Layout514,
     "5.1.4",
     "5.1.4",
     "5.1.4",
     "5.1.4",
     {"FL", "FR", "FC", "LFE", "Ls", "Rs", "TFL", "TFR", "TBL", "TBR"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
      AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT, AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT,
      AV_CHAN_TOP_BACK_LEFT, AV_CHAN_TOP_BACK_RIGHT},
     10,
     0x0002d60fu},
    {LAVOpenJocOutputPolicy::Layout712,
     "7.1.2",
     "7.1.2",
     "7.1.2",
     "7.1.2",
     {"FL", "FR", "FC", "LFE", "Lb", "Rb", "Ls", "Rs", "TFL", "TFR"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
      AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT,
      AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT},
     10,
     0x0000563fu},
    {LAVOpenJocOutputPolicy::Layout714,
     "7.1.4",
     "7.1.4",
     "7.1.4",
     "7.1.4",
     {"FL", "FR", "FC", "LFE", "Lb", "Rb", "Ls", "Rs", "TFL", "TFR", "TBL", "TBR"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER, AV_CHAN_LOW_FREQUENCY,
      AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT,
      AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_RIGHT, AV_CHAN_TOP_BACK_LEFT,
      AV_CHAN_TOP_BACK_RIGHT},
     12,
     0x0002d63fu},
    {LAVOpenJocOutputPolicy::Binaural,
     "Binaural (Headphones)",
     "7.1.4",
     "binaural",
     "stereo",
     {"Left Ear", "Right Ear"},
     {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT},
     2,
     0x00000003u},
}};

std::uint32_t popcount(std::uint64_t value)
{
    std::uint32_t count = 0;
    while (value != 0)
    {
        count += static_cast<std::uint32_t>(value & 1u);
        value >>= 1;
    }
    return count;
}

const char *ffmpeg_label_for(const AVChannel channel)
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

void test_schema_and_wire_values_are_fixed()
{
    assert(LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION == 1);
    assert(sizeof(LAVOpenJocOutputPolicy) == sizeof(std::uint32_t));
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Stereo) == 0);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout51) == 1);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout71) == 2);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout512) == 3);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout514) == 4);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout712) == 5);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Layout714) == 6);
    assert(static_cast<std::uint32_t>(LAVOpenJocOutputPolicy::Binaural) == 7);
    assert(LAV_OPENJOC_OUTPUT_CONTRACT_COUNT == 8);
}

void test_binaural_uses_ear_labels_with_stereo_transport()
{
    const LAVOpenJocOutputContract *contract =
        FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Binaural);
    assert(contract != nullptr);
    assert(std::strcmp(contract->property_page_label, "Binaural (Headphones)") == 0);
    assert(std::strcmp(contract->abi_preset_name, "7.1.4") == 0);
    assert(contract->channel_count == 2);
    assert(contract->ffmpeg_channel_mask == 0x00000003u);
    assert(contract->frame_channel_labels != nullptr);
    assert(std::strcmp(contract->frame_channel_labels[0], "BIL") == 0);
    assert(std::strcmp(contract->frame_channel_labels[1], "BIR") == 0);

    constexpr std::size_t sample_count = 64;
    const char *frame_labels[] = {"BIL", "BIR"};
    std::size_t element_count = 0;
    std::size_t byte_count = 0;
    assert(ValidateLAVOpenJocFrameMetadata(
        *contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, 2, sample_count,
        sample_count * 2, "binaural", frame_labels, 2, &element_count, &byte_count));
    assert(element_count == sample_count * 2);
    assert(byte_count == sample_count * 2 * sizeof(float));

    const char *physical_labels[] = {"FL", "FR"};
    assert(!ValidateLAVOpenJocFrameMetadata(
        *contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, 2, sample_count,
        sample_count * 2, "binaural", physical_labels, 2, nullptr, nullptr));
}

void test_every_policy_has_the_exact_canonical_contract()
{
    for (const ExpectedContract &expected : kExpectedContracts)
    {
        const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(expected.policy);
        assert(contract != nullptr);
        assert(contract == FindLAVOpenJocOutputContract(expected.policy));
        assert(contract->policy == expected.policy);
        assert(std::strcmp(contract->property_page_label, expected.property_page_label) == 0);
        if (expected.abi_preset_name == nullptr)
            assert(contract->abi_preset_name == nullptr);
        else
            assert(std::strcmp(contract->abi_preset_name, expected.abi_preset_name) == 0);
        assert(std::strcmp(contract->openjoc_layout_name, expected.openjoc_layout_name) == 0);
        assert(std::strcmp(contract->ffmpeg_standard_layout_name, expected.ffmpeg_standard_layout_name) == 0);
        assert(contract->channel_count == expected.channel_count);
        assert(contract->ffmpeg_channel_mask == expected.mask);
        assert(contract->windows_channel_mask == expected.mask);

        for (std::size_t index = 0; index < contract->channel_count; ++index)
        {
            assert(std::strcmp(contract->openjoc_semantic_labels[index], expected.semantic_labels[index]) == 0);
            assert(contract->ordered_channels[index] == expected.ordered_channels[index]);
        }
    }
}

void test_masks_and_orders_are_exact_native_windows_order()
{
    constexpr std::uint64_t kMappedWindowsBits = 0x0003ffffu;

    for (const ExpectedContract &expected : kExpectedContracts)
    {
        const LAVOpenJocOutputContract &contract = *FindLAVOpenJocOutputContract(expected.policy);
        assert(contract.ffmpeg_channel_mask == contract.windows_channel_mask);
        assert(contract.windows_channel_mask != 0);
        assert((contract.ffmpeg_channel_mask & ~kMappedWindowsBits) == 0);
        assert(popcount(contract.ffmpeg_channel_mask) == contract.channel_count);

        std::size_t channel_index = 0;
        for (std::uint32_t bit = 0; bit < 64; ++bit)
        {
            if ((contract.ffmpeg_channel_mask & (std::uint64_t{1} << bit)) == 0)
                continue;
            assert(channel_index < contract.channel_count);
            assert(contract.ordered_channels[channel_index] == static_cast<AVChannel>(bit));
            ++channel_index;
        }
        assert(channel_index == contract.channel_count);
    }
}

void test_multichannel_candidates_have_one_logical_lfe()
{
    for (const ExpectedContract &expected : kExpectedContracts)
    {
        const LAVOpenJocOutputContract &contract = *FindLAVOpenJocOutputContract(expected.policy);
        std::uint32_t lfe_channels = 0;
        for (std::size_t index = 0; index < contract.channel_count; ++index)
        {
            if (contract.ordered_channels[index] == AV_CHAN_LOW_FREQUENCY)
                ++lfe_channels;
            assert(contract.ordered_channels[index] != AV_CHAN_LOW_FREQUENCY_2);
        }

        if (expected.policy == LAVOpenJocOutputPolicy::Stereo ||
            expected.policy == LAVOpenJocOutputPolicy::Binaural)
            assert(lfe_channels == 0);
        else
            assert(lfe_channels == 1);
    }
}

void test_unknown_and_unrepresentable_policy_values_are_rejected()
{
    // These wire values stand in for Auto, 5.2.4, 7.1.6, 9.x, 22.2, and corrupt persistence.
    constexpr std::uint32_t rejected_values[] = {8u, 9u, 10u, 14u, 16u, 24u,
                                                  (std::numeric_limits<std::uint32_t>::max)()};
    for (const std::uint32_t value : rejected_values)
    {
        assert(FindLAVOpenJocOutputContract(static_cast<LAVOpenJocOutputPolicy>(value)) == nullptr);
    }
}

void test_every_contract_builds_the_exact_native_ffmpeg_layout()
{
    for (const ExpectedContract &expected : kExpectedContracts)
    {
        const LAVOpenJocOutputContract &contract = *FindLAVOpenJocOutputContract(expected.policy);
        AVChannelLayout actual{};
        assert(BuildOpenJocAvChannelLayout(contract, &actual));
        assert(av_channel_layout_check(&actual) == 1);
        assert(actual.order == AV_CHANNEL_ORDER_NATIVE);
        assert(actual.nb_channels == static_cast<int>(contract.channel_count));
        assert(actual.u.mask == contract.ffmpeg_channel_mask);
        for (std::uint32_t index = 0; index < contract.channel_count; ++index)
            assert(av_channel_layout_channel_from_index(&actual, index) == contract.ordered_channels[index]);

        AVChannelLayout parsed{};
        assert(av_channel_layout_from_string(&parsed, contract.ffmpeg_standard_layout_name) == 0);
        assert(av_channel_layout_compare(&actual, &parsed) == 0);
        av_channel_layout_uninit(&parsed);
        av_channel_layout_uninit(&actual);
    }
}

void test_openjoc_5_1_is_ffmpeg_5_1_side_not_back()
{
    const LAVOpenJocOutputContract &contract =
        *FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout51);
    AVChannelLayout actual{};
    AVChannelLayout side{};
    AVChannelLayout back{};
    assert(BuildOpenJocAvChannelLayout(contract, &actual));
    assert(av_channel_layout_from_string(&side, "5.1(side)") == 0);
    assert(av_channel_layout_from_string(&back, "5.1") == 0);
    assert(av_channel_layout_compare(&actual, &side) == 0);
    assert(av_channel_layout_compare(&actual, &back) == 1);
    av_channel_layout_uninit(&back);
    av_channel_layout_uninit(&side);
    av_channel_layout_uninit(&actual);
}

void test_invalid_contracts_leave_no_partial_ffmpeg_layout()
{
    const LAVOpenJocOutputContract &valid =
        *FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout51);

    std::array<LAVOpenJocOutputContract, 4> invalid = {valid, valid, valid, valid};
    invalid[0].ffmpeg_channel_mask = 0;
    invalid[0].windows_channel_mask = 0;
    invalid[1].channel_count += 1;
    invalid[2].ffmpeg_channel_mask |= (std::uint64_t{1} << 63);
    invalid[3].windows_channel_mask ^= 1u;

    for (const LAVOpenJocOutputContract &contract : invalid)
    {
        AVChannelLayout output{};
        assert(av_channel_layout_from_mask(&output, AV_CH_LAYOUT_STEREO) == 0);
        assert(!BuildOpenJocAvChannelLayout(contract, &output));
        assert(output.order == AV_CHANNEL_ORDER_UNSPEC);
        assert(output.nb_channels == 0);
    }
    assert(!BuildOpenJocAvChannelLayout(valid, nullptr));
}

void test_frame_metadata_validation_is_exact_and_checked()
{
    const LAVOpenJocOutputContract &contract =
        *FindLAVOpenJocOutputContract(LAVOpenJocOutputPolicy::Layout714);
    constexpr std::size_t sample_count = 256;
    const std::size_t data_len = sample_count * contract.channel_count;
    std::size_t validated_elements = 0;
    std::size_t validated_bytes = 0;
    std::array<const char *, kMaximumChannels> ffmpeg_labels{};
    for (std::uint32_t index = 0; index < contract.channel_count; ++index)
        ffmpeg_labels[index] = ffmpeg_label_for(contract.ordered_channels[index]);

    assert(ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count,
        &validated_elements, &validated_bytes));
    assert(validated_elements == data_len);
    assert(validated_bytes == data_len * sizeof(float));

    std::array<const char *, kMaximumChannels> swapped_labels{};
    for (std::uint32_t index = 0; index < contract.channel_count; ++index)
        swapped_labels[index] = ffmpeg_labels[index];
    const char *first = swapped_labels[0];
    swapped_labels[0] = swapped_labels[1];
    swapped_labels[1] = first;

    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, 0, 48000, contract.channel_count, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 44100, contract.channel_count, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, 0, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count - 1, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, 0, 0,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len - 1,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len,
        nullptr, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len,
        "7.1.2", ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, nullptr, contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, swapped_labels.data(), contract.channel_count, nullptr, nullptr));
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, sample_count, data_len,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count - 1, nullptr,
        nullptr));

    const std::size_t element_overflow_samples =
        (std::numeric_limits<std::size_t>::max)() / contract.channel_count + 1;
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, element_overflow_samples, 0,
        contract.ffmpeg_standard_layout_name, ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
    const std::size_t byte_overflow_samples =
        (std::numeric_limits<std::size_t>::max)() / (contract.channel_count * sizeof(float)) + 1;
    assert(!ValidateLAVOpenJocFrameMetadata(
        contract, LAV_OPENJOC_SAMPLE_FORMAT_FLOAT32, 48000, contract.channel_count, byte_overflow_samples,
        byte_overflow_samples * contract.channel_count, contract.ffmpeg_standard_layout_name,
        ffmpeg_labels.data(), contract.channel_count, nullptr, nullptr));
}

void test_lav_handoff_preparation_preserves_exact_layouts_and_checked_bytes()
{
    constexpr std::size_t sample_count = 1024;
    for (const ExpectedContract &expected : kExpectedContracts)
    {
        const LAVOpenJocOutputContract *contract = FindLAVOpenJocOutputContract(expected.policy);
        const std::size_t element_count = sample_count * contract->channel_count;
        AVChannelLayout layout{};
        std::uint32_t prepared_samples = 0;
        std::uint32_t prepared_bytes = 0;

        assert(PrepareLAVOpenJocFrameHandoff(
            contract, contract, 48000, contract->channel_count, sample_count, element_count, &layout,
            &prepared_samples, &prepared_bytes));
        assert(prepared_samples == sample_count);
        assert(prepared_bytes == element_count * sizeof(float));
        assert(layout.order == AV_CHANNEL_ORDER_NATIVE);
        assert(layout.nb_channels == static_cast<int>(contract->channel_count));
        assert(layout.u.mask == contract->ffmpeg_channel_mask);
        for (std::uint32_t index = 0; index < contract->channel_count; ++index)
            assert(av_channel_layout_channel_from_index(&layout, index) == contract->ordered_channels[index]);
        av_channel_layout_uninit(&layout);
    }
}

void test_decode_openjoc_source_cannot_restore_count_default_or_eight_channel_cap()
{
    const std::filesystem::path source_path =
        std::filesystem::path(__FILE__).parent_path() / "LAVAudio.cpp";
    std::ifstream source_file(source_path, std::ios::binary);
    assert(source_file);
    const std::string source{std::istreambuf_iterator<char>(source_file),
                             std::istreambuf_iterator<char>()};
    const std::size_t signature = source.find("HRESULT CLAVAudio::DecodeOpenJoc(HRESULT *hrDeliver)");
    assert(signature != std::string::npos);
    const std::size_t opening_brace = source.find('{', signature);
    assert(opening_brace != std::string::npos);

    std::size_t closing_brace = std::string::npos;
    std::size_t depth = 0;
    for (std::size_t index = opening_brace; index < source.size(); ++index)
    {
        if (source[index] == '{')
            ++depth;
        else if (source[index] == '}' && --depth == 0)
        {
            closing_brace = index;
            break;
        }
    }
    assert(closing_brace != std::string::npos);

    std::string body = source.substr(opening_brace, closing_brace - opening_brace + 1);
    body.erase(std::remove_if(body.begin(), body.end(), [](const unsigned char value) {
                   return std::isspace(value) != 0;
               }),
               body.end());
    assert(body.find("PrepareLAVOpenJocFrameHandoff") != std::string::npos);
    assert(body.find("channel_count>8") == std::string::npos);
    assert(body.find("av_channel_layout_default") == std::string::npos);
}
} // namespace

int wmain()
{
    test_schema_and_wire_values_are_fixed();
    test_binaural_uses_ear_labels_with_stereo_transport();
    test_every_policy_has_the_exact_canonical_contract();
    test_masks_and_orders_are_exact_native_windows_order();
    test_multichannel_candidates_have_one_logical_lfe();
    test_unknown_and_unrepresentable_policy_values_are_rejected();
    test_every_contract_builds_the_exact_native_ffmpeg_layout();
    test_openjoc_5_1_is_ffmpeg_5_1_side_not_back();
    test_invalid_contracts_leave_no_partial_ffmpeg_layout();
    test_frame_metadata_validation_is_exact_and_checked();
    test_lav_handoff_preparation_preserves_exact_layouts_and_checked_bytes();
    test_decode_openjoc_source_cannot_restore_count_default_or_eight_channel_cap();
    return 0;
}
