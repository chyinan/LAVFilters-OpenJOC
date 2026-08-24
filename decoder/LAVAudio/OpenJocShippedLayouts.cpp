/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocShippedLayouts.h"
#include "OpenJocOutput.h"

#include <array>

namespace
{
constexpr std::array<LAVOpenJocOutputPolicy, LAV_OPENJOC_OUTPUT_CONTRACT_COUNT>
    kShippedOutputPolicies = {
    LAVOpenJocOutputPolicy::Stereo,
    LAVOpenJocOutputPolicy::Layout51,
    LAVOpenJocOutputPolicy::Layout71,
    LAVOpenJocOutputPolicy::Layout512,
    LAVOpenJocOutputPolicy::Layout514,
    LAVOpenJocOutputPolicy::Layout712,
    LAVOpenJocOutputPolicy::Layout714,
};
}

const LAVOpenJocOutputPolicy *GetLAVOpenJocShippedOutputPolicies(std::size_t *count) noexcept
{
    if (count)
        *count = kShippedOutputPolicies.size();
    return kShippedOutputPolicies.data();
}

bool IsLAVOpenJocOutputPolicyShipped(const LAVOpenJocOutputPolicy policy) noexcept
{
    for (const LAVOpenJocOutputPolicy shipped : kShippedOutputPolicies)
    {
        if (shipped == policy)
            return true;
    }
    return false;
}
