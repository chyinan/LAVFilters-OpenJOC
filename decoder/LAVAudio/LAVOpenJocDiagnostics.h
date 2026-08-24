/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#pragma once

#include <guiddef.h>
#include <unknwn.h>

// Read-only diagnostics for the live OpenJOC decoder owned by one connected
// LAV Audio instance. This is a separate interface so existing public vtables
// and IIDs remain unchanged.
// {16C95FF3-9D9E-4282-AF61-E6C7AF32446B}
DEFINE_GUID(IID_ILAVOpenJocDiagnostics, 0x16c95ff3, 0x9d9e, 0x4282, 0xaf, 0x61, 0xe6, 0xc7,
            0xaf, 0x32, 0x44, 0x6b);

interface __declspec(uuid("16C95FF3-9D9E-4282-AF61-E6C7AF32446B")) ILAVOpenJocDiagnostics
    : public IUnknown
{
    STDMETHOD(GetOpenJocInputByteCounts)
    (ULONGLONG * classifier_input_bytes, ULONGLONG * stream_input_bytes) = 0;
};
