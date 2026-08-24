/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include <cstdint>
#include <guiddef.h>
#include <unknwn.h>

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

// {6B97FD1C-B463-4B5E-9349-CD8B964D6B46}
DEFINE_GUID(IID_ILAVOpenJocSettings, 0x6b97fd1c, 0xb463, 0x4b5e, 0x93, 0x49, 0xcd, 0x8b, 0x96, 0x4d, 0x6b, 0x46);

interface __declspec(uuid("6B97FD1C-B463-4B5E-9349-CD8B964D6B46")) ILAVOpenJocSettings : public IUnknown
{
    STDMETHOD(GetOutputPolicy)(LAVOpenJocOutputPolicy * policy) = 0;
    STDMETHOD(SetOutputPolicy)(LAVOpenJocOutputPolicy policy) = 0;
};
