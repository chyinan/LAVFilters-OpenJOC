/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Structured OpenJOC fallback diagnostic tests.

// pattern: Functional Core

#include "OpenJocDiagnostic.h"

#include <cassert>
#include <cstring>
#include <cstdlib>
#include <string>
#include <windows.h>

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

void test_bounded_detail_does_not_read_past_an_unterminated_buffer()
{
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page_size = system_info.dwPageSize;
    assert(page_size > LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES);

    void *region = VirtualAlloc(nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(region != nullptr);
    char *detail = static_cast<char *>(region) + page_size - LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES;
    std::memset(detail, 'x', LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES);

    DWORD previous_protection = 0;
    const BOOL protected_guard = VirtualProtect(static_cast<char *>(region) + page_size, page_size,
                                                PAGE_NOACCESS, &previous_protection);
    assert(protected_guard != FALSE);

    bool completed = false;
    __try
    {
        const std::string bounded = BoundLAVOpenJocDiagnosticDetail(detail);
        completed = bounded.size() == LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        completed = false;
    }

    VirtualFree(region, 0, MEM_RELEASE);
    assert(completed);
    if (!completed)
        std::abort();
}
} // namespace

int wmain()
{
    test_normal_stock_has_no_failure_reason();
    test_classifier_validation_failure_is_unsupported_profile();
    test_structural_and_metadata_failures_are_distinct();
    test_unrecognized_probe_failure_is_generic_decode_error();
    test_details_are_bounded_and_single_line();
    test_bounded_detail_does_not_read_past_an_unterminated_buffer();
    return 0;
}
