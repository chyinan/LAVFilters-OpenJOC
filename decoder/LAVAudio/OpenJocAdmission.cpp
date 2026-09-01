/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocAdmission.h"

std::size_t LAVOpenJocAdmission::classification_offset(const std::size_t buffered_bytes) const
{
    if (m_state != LAVOpenJocState::Undecided)
        return 0;

    return m_classified_bytes < buffered_bytes ? m_classified_bytes : buffered_bytes;
}

void LAVOpenJocAdmission::note_classified(const std::size_t buffered_bytes)
{
    if (m_state != LAVOpenJocState::Undecided || buffered_bytes < m_classified_bytes ||
        buffered_bytes > MaxRetainedBytes)
    {
        return;
    }

    m_classified_bytes = buffered_bytes;
}

LAVOpenJocAdmissionAction LAVOpenJocAdmission::resolve(const LAVOpenJocClassification classification,
                                                        const std::size_t buffered_bytes)
{
    if (m_state != LAVOpenJocState::Undecided)
        return {};

    if (buffered_bytes < m_classified_bytes || buffered_bytes > MaxRetainedBytes)
    {
        m_state = LAVOpenJocState::StockAfterOpenJocFailure;
        m_classified_bytes = 0;
        return {LAVOpenJocActionKind::UseStockDecoder, 0};
    }

    if (classification == LAVOpenJocClassification::ConfirmedNonJoc)
    {
        m_state = LAVOpenJocState::StockCodec;
        m_classified_bytes = 0;
        return {LAVOpenJocActionKind::UseStockDecoder, 0};
    }

    if (classification == LAVOpenJocClassification::InvalidOrUnsupported)
    {
        m_state = LAVOpenJocState::StockAfterOpenJocFailure;
        m_classified_bytes = 0;
        return {LAVOpenJocActionKind::UseStockDecoder, 0};
    }

    if (classification == LAVOpenJocClassification::ConfirmedJoc)
    {
        m_state = LAVOpenJocState::OpenJoc;
        m_classified_bytes = buffered_bytes;
        return {LAVOpenJocActionKind::PromoteToOpenJoc, buffered_bytes};
    }

    return {};
}

void LAVOpenJocAdmission::reset()
{
    m_state = LAVOpenJocState::Undecided;
    m_classified_bytes = 0;
}
