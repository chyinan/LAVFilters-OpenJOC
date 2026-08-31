/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#include "OpenJocCandidate.h"

bool IsLAVOpenJocCandidate(const AVCodecID codec, const bool has_bitstream_context,
                           const bool available, const bool is_spdif_subtype)
{
    return available && !has_bitstream_context && !is_spdif_subtype &&
           (codec == AV_CODEC_ID_AC3 || codec == AV_CODEC_ID_EAC3);
}
