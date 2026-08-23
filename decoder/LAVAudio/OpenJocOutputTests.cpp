/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Canonical OpenJOC output-policy contract tests.

#include "LAVOpenJocSettings.h"
#include "OpenJocOutput.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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
static_assert(LAV_OPENJOC_OUTPUT_CONTRACT_COUNT == 7);
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
     "Stereo",
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
    assert(LAV_OPENJOC_OUTPUT_CONTRACT_COUNT == 7);
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

        if (expected.policy == LAVOpenJocOutputPolicy::Stereo)
            assert(lfe_channels == 0);
        else
            assert(lfe_channels == 1);
    }
}

void test_unknown_and_unrepresentable_policy_values_are_rejected()
{
    // These wire values stand in for Auto, 5.2.4, 7.1.6, 9.x, 22.2, and corrupt persistence.
    constexpr std::uint32_t rejected_values[] = {7u, 8u, 9u, 10u, 14u, 16u, 24u,
                                                  (std::numeric_limits<std::uint32_t>::max)()};
    for (const std::uint32_t value : rejected_values)
    {
        assert(FindLAVOpenJocOutputContract(static_cast<LAVOpenJocOutputPolicy>(value)) == nullptr);
    }
}
} // namespace

int wmain()
{
    test_schema_and_wire_values_are_fixed();
    test_every_policy_has_the_exact_canonical_contract();
    test_masks_and_orders_are_exact_native_windows_order();
    test_multichannel_candidates_have_one_logical_lfe();
    test_unknown_and_unrepresentable_policy_values_are_rejected();
    return 0;
}
