/*
 *      Copyright (C) 2010-2021 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * OpenJOC downstream modification (openjoc-0.10.0, 2026-08-22):
 * add a compile-time side-by-side display name for the OpenJOC-enabled filter.
 */

#pragma once

// Set minimal target OS (7+)
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0601
#ifdef WINVER
#undef WINVER
#endif
#define WINVER _WIN32_WINNT

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN

#define LAV_AUDIO "LAV Audio Decoder"
#if defined(LAV_OPENJOC_SIDE_BY_SIDE)
#define LAV_AUDIO_DISPLAY_NAME L"LAV Audio Decoder (OpenJOC)"
#else
#define LAV_AUDIO_DISPLAY_NAME L"LAV Audio Decoder"
#endif
#define LAV_VIDEO "LAV Video Decoder"
#define LAV_SPLITTER "LAV Splitter"
