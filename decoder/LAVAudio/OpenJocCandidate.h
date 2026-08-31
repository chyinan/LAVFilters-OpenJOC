/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

extern "C"
{
#include "libavcodec/codec_id.h"
}

bool IsLAVOpenJocCandidate(AVCodecID codec, bool has_bitstream_context,
                           bool available, bool is_spdif_subtype);
