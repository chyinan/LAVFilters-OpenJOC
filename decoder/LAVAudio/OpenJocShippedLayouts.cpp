/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocShippedLayouts.h"

#include <array>

namespace
{
constexpr std::array<LAVOpenJocOutputPolicy, 1> kShippedOutputPolicies = {
    LAVOpenJocOutputPolicy::Stereo,
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
