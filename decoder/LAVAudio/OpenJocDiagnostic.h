/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "OpenJocAdmission.h"

#include <cstddef>
#include <string>

inline constexpr std::size_t LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES = 512;

enum class LAVOpenJocFailureReason
{
    None,
    MalformedJocMetadata,
    UnsupportedJocProfile,
    InvalidJocCarriage,
    OpenJocDecodeError,
    UnsupportedOutputLayout,
    BinauralHrtfConfiguration,
};

struct LAVOpenJocDiagnosticSnapshot
{
    bool warning = false;
    LAVOpenJocFailureReason reason = LAVOpenJocFailureReason::None;
    bool failure_au_known = false;
    std::size_t failure_au = 0;
    std::string detail;
};

[[nodiscard]] LAVOpenJocFailureReason
ClassifyLAVOpenJocProbeFailure(LAVOpenJocClassification classification,
                               bool classifier_call_failed, const char *detail) noexcept;

[[nodiscard]] std::string BoundLAVOpenJocDiagnosticDetail(const char *detail);

[[nodiscard]] const char *LAVOpenJocFailureReasonLabel(LAVOpenJocFailureReason reason) noexcept;

[[nodiscard]] LAVOpenJocDiagnosticSnapshot MakeLAVOpenJocFallbackDiagnostic(
    LAVOpenJocFailureReason reason, const char *detail, bool failure_au_known,
    std::size_t failure_au);

[[nodiscard]] LAVOpenJocDiagnosticSnapshot MakeLAVOpenJocRuntimeDiagnostic(
    LAVOpenJocFailureReason reason, const char *detail);
