/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "LAVOpenJocSettings.h"

#include <cstdint>
#include <windows.h>

inline constexpr std::uint32_t LAV_OPENJOC_BINAURAL_SETTINGS_SCHEMA_VERSION = 1;
inline constexpr DWORD LAV_OPENJOC_BINAURAL_SETTINGS_TEXT_CAPACITY = 32768;

enum class LAVOpenJocHrtfSource : std::uint32_t
{
    BuiltinSadieIiD1 = 0,
    CustomSofa = 1,
};

enum class LAVOpenJocBinauralVirtualLayout : std::uint32_t
{
    Layout714 = 0,
    Layout916 = 1,
};

inline constexpr bool IsLAVOpenJocHrtfSource(const LAVOpenJocHrtfSource source) noexcept
{
    return source == LAVOpenJocHrtfSource::BuiltinSadieIiD1 || source == LAVOpenJocHrtfSource::CustomSofa;
}

inline constexpr bool IsLAVOpenJocBinauralVirtualLayout(
    const LAVOpenJocBinauralVirtualLayout layout) noexcept
{
    return layout == LAVOpenJocBinauralVirtualLayout::Layout714 ||
           layout == LAVOpenJocBinauralVirtualLayout::Layout916;
}

// {A4DA1C8C-3D27-4D11-8A0D-B4D03F5D21C2}
DEFINE_GUID(IID_ILAVOpenJocBinauralSettings, 0xa4da1c8c, 0x3d27, 0x4d11, 0x8a, 0x0d, 0xb4, 0xd0, 0x3f, 0x5d,
            0x21, 0xc2);

interface __declspec(uuid("A4DA1C8C-3D27-4D11-8A0D-B4D03F5D21C2")) ILAVOpenJocBinauralSettings : public IUnknown
{
    STDMETHOD(GetBinauralHrtfSource)(LAVOpenJocHrtfSource * source) = 0;
    STDMETHOD(GetBinauralVirtualLayout)(LAVOpenJocBinauralVirtualLayout * layout) = 0;
    STDMETHOD(GetCustomSofaPath)(LPWSTR path, DWORD capacity) = 0;
    STDMETHOD(SetBinauralConfiguration)(LAVOpenJocOutputPolicy output_policy, LAVOpenJocHrtfSource source,
                                         LAVOpenJocBinauralVirtualLayout layout, LPCWSTR sofa_path) = 0;
    STDMETHOD(GetBinauralConfigurationError)(LPWSTR detail, DWORD capacity) = 0;
};

