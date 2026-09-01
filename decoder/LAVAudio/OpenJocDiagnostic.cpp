/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocDiagnostic.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace
{
const char *DefaultDetail(const LAVOpenJocFailureReason reason) noexcept
{
    switch (reason)
    {
    case LAVOpenJocFailureReason::MalformedJocMetadata:
        return "OpenJOC rejected malformed JOC metadata";
    case LAVOpenJocFailureReason::UnsupportedJocProfile:
        return "OpenJOC rejected an unsupported JOC profile";
    case LAVOpenJocFailureReason::InvalidJocCarriage:
        return "OpenJOC rejected invalid JOC carriage";
    case LAVOpenJocFailureReason::OpenJocDecodeError:
        return "OpenJOC decoding failed";
    case LAVOpenJocFailureReason::UnsupportedOutputLayout:
        return "the selected output layout was rejected by the downstream filter";
    case LAVOpenJocFailureReason::BinauralHrtfConfiguration:
        return "the selected Binaural HRTF configuration was rejected";
    case LAVOpenJocFailureReason::None:
    default:
        return "";
    }
}

bool ContainsInsensitive(const std::string_view text, const std::string_view needle) noexcept
{
    if (needle.empty() || needle.size() > text.size())
        return false;
    for (std::size_t offset = 0; offset + needle.size() <= text.size(); ++offset)
    {
        bool matches = true;
        for (std::size_t index = 0; index < needle.size(); ++index)
        {
            const auto left = static_cast<unsigned char>(text[offset + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right))
            {
                matches = false;
                break;
            }
        }
        if (matches)
            return true;
    }
    return false;
}
} // namespace

LAVOpenJocFailureReason
ClassifyLAVOpenJocProbeFailure(const LAVOpenJocClassification classification,
                               const bool classifier_call_failed, const char *detail) noexcept
{
    if (classification != LAVOpenJocClassification::InvalidOrUnsupported)
        return LAVOpenJocFailureReason::None;
    if (!classifier_call_failed)
        return LAVOpenJocFailureReason::UnsupportedJocProfile;

    const std::string_view message = detail ? std::string_view(detail) : std::string_view{};
    if (ContainsInsensitive(message, "emdf") || ContainsInsensitive(message, "metadata"))
        return LAVOpenJocFailureReason::MalformedJocMetadata;
    if (ContainsInsensitive(message, "dependent") || ContainsInsensitive(message, "access unit") ||
        ContainsInsensitive(message, "syncframe") || ContainsInsensitive(message, "carriage"))
        return LAVOpenJocFailureReason::InvalidJocCarriage;
    return LAVOpenJocFailureReason::OpenJocDecodeError;
}

std::string BoundLAVOpenJocDiagnosticDetail(const char *detail)
{
    if (!detail)
        return {};

    std::string result;
    result.reserve(std::min<std::size_t>(
        std::char_traits<char>::length(detail), LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES));
    for (const char *cursor = detail;
         *cursor != '\0' && result.size() < LAV_OPENJOC_MAX_DIAGNOSTIC_DETAIL_BYTES; ++cursor)
    {
        const unsigned char value = static_cast<unsigned char>(*cursor);
        result.push_back(value == '\r' || value == '\n' || value == '\t' ? ' ' : static_cast<char>(value));
    }
    return result;
}

const char *LAVOpenJocFailureReasonLabel(const LAVOpenJocFailureReason reason) noexcept
{
    switch (reason)
    {
    case LAVOpenJocFailureReason::MalformedJocMetadata: return "Malformed JOC metadata";
    case LAVOpenJocFailureReason::UnsupportedJocProfile: return "Unsupported JOC profile";
    case LAVOpenJocFailureReason::InvalidJocCarriage: return "Invalid JOC carriage";
    case LAVOpenJocFailureReason::OpenJocDecodeError: return "OpenJOC decode error";
    case LAVOpenJocFailureReason::UnsupportedOutputLayout: return "Unsupported output layout";
    case LAVOpenJocFailureReason::BinauralHrtfConfiguration: return "Binaural HRTF configuration";
    case LAVOpenJocFailureReason::None:
    default: return "";
    }
}

LAVOpenJocDiagnosticSnapshot MakeLAVOpenJocFallbackDiagnostic(
    const LAVOpenJocFailureReason reason, const char *detail,
    const bool failure_au_known, const std::size_t failure_au)
{
    LAVOpenJocDiagnosticSnapshot snapshot;
    snapshot.warning = reason != LAVOpenJocFailureReason::None;
    snapshot.reason = reason;
    snapshot.failure_au_known = snapshot.warning && failure_au_known;
    snapshot.failure_au = failure_au;
    snapshot.detail = BoundLAVOpenJocDiagnosticDetail(detail);
    if (snapshot.warning && snapshot.detail.empty())
        snapshot.detail = DefaultDetail(reason);
    return snapshot;
}

LAVOpenJocDiagnosticSnapshot MakeLAVOpenJocRuntimeDiagnostic(
    const LAVOpenJocFailureReason reason, const char *detail)
{
    return MakeLAVOpenJocFallbackDiagnostic(reason, detail, false, 0);
}
