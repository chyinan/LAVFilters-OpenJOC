/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include <cstdint>

inline constexpr std::uint32_t LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION = 1;

enum class LAVOpenJocOutputPolicy : std::uint32_t
{
    Stereo = 0,
    Layout51 = 1,
    Layout71 = 2,
    Layout512 = 3,
    Layout514 = 4,
    Layout712 = 5,
    Layout714 = 6,
};

static_assert(sizeof(LAVOpenJocOutputPolicy) == sizeof(std::uint32_t));
