/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include <cstddef>

enum class LAVOpenJocState
{
    Undecided,
    StockEac3,
    OpenJoc,
};

enum class LAVOpenJocClassification
{
    Unknown,
    ConfirmedJoc,
    ConfirmedNonJoc,
    InvalidOrUnsupported,
};

enum class LAVOpenJocActionKind
{
    NoAction,
    UseStockEac3,
    PromoteToOpenJoc,
};

struct LAVOpenJocAdmissionAction
{
    LAVOpenJocActionKind kind = LAVOpenJocActionKind::NoAction;
    std::size_t bytes_to_feed = 0;
};

class LAVOpenJocAdmission
{
  public:
    static constexpr std::size_t MaxRetainedBytes = 131072;

    LAVOpenJocState state() const { return m_state; }
    std::size_t classified_bytes() const { return m_classified_bytes; }

    std::size_t classification_offset(std::size_t buffered_bytes) const;
    void note_classified(std::size_t buffered_bytes);
    LAVOpenJocAdmissionAction resolve(LAVOpenJocClassification classification, std::size_t buffered_bytes);
    void reset();

  private:
    LAVOpenJocState m_state = LAVOpenJocState::Undecided;
    std::size_t m_classified_bytes = 0;
};
