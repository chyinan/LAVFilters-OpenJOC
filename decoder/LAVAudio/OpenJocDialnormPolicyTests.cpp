/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocDialnorm.h"

#include <cassert>
#include <cstdint>

int main()
{
    static_assert(sizeof(LAVOpenJocDialnormPolicy) == sizeof(std::uint32_t));
    static_assert(static_cast<std::uint32_t>(LAVOpenJocDialnormPolicy::Calibrated) == 0);
    static_assert(static_cast<std::uint32_t>(LAVOpenJocDialnormPolicy::UnityCompatibility) == 1);
    static_assert(LAV_OPENJOC_DIALNORM_POLICY_SCHEMA_VERSION == 1);

    std::uint32_t mode = 0xffffffffu;
    assert(TryMapLAVOpenJocDialnormPolicy(LAVOpenJocDialnormPolicy::Calibrated, &mode));
    assert(mode == OPENJOC_DIALNORM_DEFAULT);

    mode = 0xffffffffu;
    assert(TryMapLAVOpenJocDialnormPolicy(LAVOpenJocDialnormPolicy::UnityCompatibility, &mode));
    assert(mode == OPENJOC_DIALNORM_ANALOG);

    mode = OPENJOC_DIALNORM_DIGITAL;
    assert(!TryMapLAVOpenJocDialnormPolicy(static_cast<LAVOpenJocDialnormPolicy>(0xffffffffu), &mode));
    assert(mode == OPENJOC_DIALNORM_DIGITAL);
    assert(!TryMapLAVOpenJocDialnormPolicy(LAVOpenJocDialnormPolicy::Calibrated, nullptr));
    return 0;
}
