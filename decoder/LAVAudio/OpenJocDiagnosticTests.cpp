/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Structured OpenJOC fallback diagnostic tests.

// pattern: Functional Core

#include "OpenJocDiagnostic.h"

#include <cassert>
#include <string>

namespace
{
void test_normal_stock_has_no_failure_reason()
{
    assert(ClassifyLAVOpenJocProbeFailure(
               LAVOpenJocClassification::ConfirmedNonJoc, false, nullptr) ==
           LAVOpenJocFailureReason::None);
}

void test_classifier_validation_failure_is_unsupported_profile()
{
    assert(ClassifyLAVOpenJocProbeFailure(
               LAVOpenJocClassification::InvalidOrUnsupported, false, nullptr) ==
           LAVOpenJocFailureReason::UnsupportedJocProfile);
}

void test_structural_and_metadata_failures_are_distinct()
{
    assert(ClassifyLAVOpenJocProbeFailure(
               LAVOpenJocClassification::InvalidOrUnsupported, true,
               "failed to decode carried EMDF: truncated EMDF container") ==
           LAVOpenJocFailureReason::MalformedJocMetadata);
    assert(ClassifyLAVOpenJocProbeFailure(
               LAVOpenJocClassification::InvalidOrUnsupported, true,
               "invalid dependent sequence in access unit") ==
           LAVOpenJocFailureReason::InvalidJocCarriage);
}

void test_unrecognized_probe_failure_is_generic_decode_error()
{
    assert(ClassifyLAVOpenJocProbeFailure(
               LAVOpenJocClassification::InvalidOrUnsupported, true,
               "decoder failed") == LAVOpenJocFailureReason::OpenJocDecodeError);
}

void test_details_are_bounded_and_single_line()
{
    const std::string long_detail(2048, 'x');
    const std::string bounded =
        BoundLAVOpenJocDiagnosticDetail("line one\nline two\rline three");
    assert(bounded == "line one line two line three");
    assert(BoundLAVOpenJocDiagnosticDetail(long_detail.c_str()).size() <=
           LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES);
}
} // namespace

int wmain()
{
    test_normal_stock_has_no_failure_reason();
    test_classifier_validation_failure_is_unsupported_profile();
    test_structural_and_metadata_failures_are_distinct();
    test_unrecognized_probe_failure_is_generic_decode_error();
    test_details_are_bounded_and_single_line();
    return 0;
}
