/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#pragma once

#include <cstddef>

typedef struct openjoc_live_inspection_snapshot openjoc_live_inspection_snapshot;

// {9F8F2E5E-4D7B-4E8B-9A5B-8E7A6A2C1F40}
DEFINE_GUID(IID_ILAVOpenJocInspection, 0x9f8f2e5e, 0x4d7b, 0x4e8b, 0x9a, 0x5b, 0x8e, 0x7a, 0x6a, 0x2c, 0x1f, 0x40);

interface __declspec(uuid("9F8F2E5E-4D7B-4E8B-9A5B-8E7A6A2C1F40")) ILAVOpenJocInspection : public IUnknown
{
    STDMETHOD(GetOpenJocLiveInspectionSnapshot)(openjoc_live_inspection_snapshot *snapshot) = 0;
    STDMETHOD(CopyOpenJocLiveInspectionJson)(char *output, std::size_t output_capacity,
                                             std::size_t *required_size) = 0;
};
