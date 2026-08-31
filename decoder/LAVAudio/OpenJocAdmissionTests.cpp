/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// OpenJOC admission state-machine tests.

#include "OpenJocAdmission.h"
#include "OpenJocCandidate.h"

#include <cassert>
#include <initializer_list>

static void test_classification_cursor_tracks_only_new_retained_bytes()
{
    LAVOpenJocAdmission admission;

    assert(admission.state() == LAVOpenJocState::Undecided);
    assert(admission.classification_offset(128) == 0);
    admission.note_classified(128);
    assert(admission.classification_offset(128) == 128);
    assert(admission.classification_offset(256) == 128);
    admission.note_classified(256);
    assert(admission.classification_offset(256) == 256);
}

static void test_positive_evidence_promotes_retained_bytes_once()
{
    LAVOpenJocAdmission admission;
    admission.note_classified(128);

    const LAVOpenJocAdmissionAction action =
        admission.resolve(LAVOpenJocClassification::ConfirmedJoc, 512);

    assert(action.kind == LAVOpenJocActionKind::PromoteToOpenJoc);
    assert(action.bytes_to_feed == 512);
    assert(admission.state() == LAVOpenJocState::OpenJoc);
    assert(admission.resolve(LAVOpenJocClassification::ConfirmedJoc, 512).kind ==
           LAVOpenJocActionKind::NoAction);
}

static void test_non_joc_and_errors_preserve_stock_path()
{
    for (const LAVOpenJocClassification classification : {
             LAVOpenJocClassification::ConfirmedNonJoc,
             LAVOpenJocClassification::InvalidOrUnsupported,
         })
    {
        LAVOpenJocAdmission admission;
        const LAVOpenJocAdmissionAction action = admission.resolve(classification, 512);
        assert(action.kind == LAVOpenJocActionKind::UseStockDecoder);
        assert(action.bytes_to_feed == 0);
        assert(admission.state() == LAVOpenJocState::StockCodec);
    }
}

static void test_retention_limit_falls_back_without_unbounded_growth()
{
    LAVOpenJocAdmission admission;
    const LAVOpenJocAdmissionAction action =
        admission.resolve(LAVOpenJocClassification::Unknown,
                          LAVOpenJocAdmission::MaxRetainedBytes + 1);

    assert(action.kind == LAVOpenJocActionKind::UseStockDecoder);
    assert(admission.state() == LAVOpenJocState::StockCodec);
}

static void test_reset_reopens_admission_for_a_new_timeline()
{
    LAVOpenJocAdmission admission;
    admission.note_classified(256);
    admission.resolve(LAVOpenJocClassification::ConfirmedJoc, 256);
    admission.reset();

    assert(admission.state() == LAVOpenJocState::Undecided);
    assert(admission.classification_offset(64) == 0);
}

static void test_only_ac3_codec_families_enter_the_openjoc_candidate_lane()
{
    assert(IsLAVOpenJocCandidate(AV_CODEC_ID_AC3, false, true, false));
    assert(IsLAVOpenJocCandidate(AV_CODEC_ID_EAC3, false, true, false));
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_AAC, false, true, false));
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_NONE, false, true, false));
}

static void test_passthrough_and_spdif_precede_openjoc_candidate_probing()
{
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_AC3, true, true, false));
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_EAC3, true, true, false));
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_AC3, false, true, true));
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_EAC3, false, true, true));
    assert(!IsLAVOpenJocCandidate(AV_CODEC_ID_AC3, false, false, false));
}

int wmain()
{
    test_classification_cursor_tracks_only_new_retained_bytes();
    test_positive_evidence_promotes_retained_bytes_once();
    test_non_joc_and_errors_preserve_stock_path();
    test_retention_limit_falls_back_without_unbounded_growth();
    test_reset_reopens_admission_for_a_new_timeline();
    test_only_ac3_codec_families_enter_the_openjoc_candidate_lane();
    test_passthrough_and_spdif_precede_openjoc_candidate_probing();
    return 0;
}
