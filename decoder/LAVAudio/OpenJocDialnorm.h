/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "LAVOpenJocSettings.h"

#include <cstdint>

inline bool IsLAVOpenJocDialnormPolicy(const LAVOpenJocDialnormPolicy policy) noexcept
{
    return policy == LAVOpenJocDialnormPolicy::Calibrated ||
           policy == LAVOpenJocDialnormPolicy::UnityCompatibility;
}

#if defined(LAV_ENABLE_OPENJOC)
#include "openjoc.h"

inline bool TryMapLAVOpenJocDialnormPolicy(const LAVOpenJocDialnormPolicy policy,
                                           std::uint32_t *const mode) noexcept
{
    if (!mode)
        return false;

    std::uint32_t mapped = 0;
    switch (policy)
    {
    case LAVOpenJocDialnormPolicy::Calibrated:
        mapped = OPENJOC_DIALNORM_DEFAULT;
        break;
    case LAVOpenJocDialnormPolicy::UnityCompatibility:
        mapped = OPENJOC_DIALNORM_ANALOG;
        break;
    default:
        return false;
    }
    *mode = mapped;
    return true;
}
#endif
