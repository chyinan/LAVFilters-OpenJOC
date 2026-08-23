/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocOutput.h"

#include <array>

namespace
{
constexpr const char *kStereoLabels[] = {"FL", "FR"};
constexpr AVChannel kStereoChannels[] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT};

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
}};
} // namespace

const LAVOpenJocOutputContract *FindLAVOpenJocOutputContract(const LAVOpenJocOutputPolicy policy) noexcept
{
    const std::uint32_t wire_value = static_cast<std::uint32_t>(policy);
    if (wire_value >= kContracts.size())
        return nullptr;

    const LAVOpenJocOutputContract &contract = kContracts[wire_value];
    return contract.policy == policy ? &contract : nullptr;
}
