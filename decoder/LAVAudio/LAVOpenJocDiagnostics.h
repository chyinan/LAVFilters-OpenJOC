/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Imperative Shell

#pragma once

#include <windows.h>
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
    // Returns S_FALSE without modifying outputs when the streaming state is
    // busy; live status callers should retry rather than wait on the stream.
    STDMETHOD(GetOpenJocInputByteCounts)
    (ULONGLONG * classifier_input_bytes, ULONGLONG * stream_input_bytes) = 0;
};

typedef enum LAVOpenJocDiagnosticReason
{
    LAVOpenJocDiagnosticNone = 0,
    LAVOpenJocDiagnosticMalformedJocMetadata = 1,
    LAVOpenJocDiagnosticUnsupportedJocProfile = 2,
    LAVOpenJocDiagnosticInvalidJocCarriage = 3,
    LAVOpenJocDiagnosticOpenJocDecodeError = 4,
    LAVOpenJocDiagnosticUnsupportedOutputLayout = 5,
} LAVOpenJocDiagnosticReason;

// {A9C07B6A-4C8F-4B6A-9D1F-6C9D3E5B7A20}
DEFINE_GUID(IID_ILAVOpenJocDiagnostics2, 0xa9c07b6a, 0x4c8f, 0x4b6a, 0x9d, 0x1f, 0x6c, 0x9d, 0x3e,
            0x5b, 0x7a, 0x20);

interface __declspec(uuid("A9C07B6A-4C8F-4B6A-9D1F-6C9D3E5B7A20")) ILAVOpenJocDiagnostics2
    : public IUnknown
{
    // The detail is copied into caller-owned UTF-16 storage. If failure_au_known
    // is FALSE, failure_au is unspecified and must be ignored. S_FALSE means
    // the streaming state is busy; callers displaying a live status page should
    // retain the previous snapshot and retry on a later UI tick.
    STDMETHOD(GetOpenJocPlaybackDiagnostics)
    (LAVOpenJocDiagnosticReason * reason, BOOL * warning, BOOL * failure_au_known,
     ULONGLONG * failure_au, LPWSTR detail, DWORD detail_capacity) = 0;
};
