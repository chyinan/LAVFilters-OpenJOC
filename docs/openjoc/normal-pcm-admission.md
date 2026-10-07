<!--
SPDX-FileCopyrightText: 2026 OpenJOC contributors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Normal PCM admission in the side-by-side filter

The OpenJOC side-by-side LAV Audio filter now advertises ordinary
`MEDIASUBTYPE_PCM` and `MEDIASUBTYPE_IEEE_FLOAT` input. The same format validator
is used before admission and codec initialization, and by the subtype-to-codec
mapping. These changes are compiled only with `LAV_OPENJOC_SIDE_BY_SIDE`.
Non-side-by-side builds retain upstream admission behavior.

This closes a filter-selection gap: with Raw SPDIF input disabled (the default),
the prior side-by-side filter did not advertise or accept these normal PCM
subtypes. An application opening a PCM-first file could therefore choose a
different decoder before the user selected the JOC track. The source defect is
verified; whether a particular player's saved graph chooses this filter still
requires inspection in that player.

The user's original private `MEDIASUBTYPE_FFMPEG_AUDIO` route was already
resolved by placing OpenJOC first in the player's filter priority. The user
confirmed PCM ↔ JOC switching on that route. This ordinary-PCM feature is
independent of that resolved configuration issue. The capture harness directly
loads the private filter; it does not prove host filter enumeration or selection.

## Format and decoding contract

- Integer PCM containers: unsigned 8-bit, signed little-endian 16/24/32-bit
- IEEE floating-point containers: little-endian 32/64-bit
- `FORMAT_WaveFormatEx`, with a matching basic wave tag or a complete
  `WAVEFORMATEXTENSIBLE` header and matching subformat
- Extensible valid bits must be nonzero and no greater than the container;
  floating-point valid bits must equal the container width
- Channel mask zero is allowed as unspecified/direct-out; a nonzero mask must
  contain one standard Windows speaker position per channel
- Channel count, sample rate, block alignment, byte rate and declared format
  length are checked before decoding; truncated and contradictory formats fail
  closed, even if Raw SPDIF input is enabled
- Basic `WAVE_FORMAT_PCM` ignores the stored `cbSize`, as specified by
  [Microsoft's WAVEFORMATEX contract](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatex).
  The actual buffer must still hold a complete `WAVEFORMATEX`; float and
  extensible formats retain their declared-extension and full-header checks

The codec is selected from **container width**, not valid precision. Packed
24-bit uses `PCM_S24LE`; 24 valid bits left-aligned in a 32-bit container uses
`PCM_S32LE`, with the 24-bit precision retained for output negotiation. The stock
LAV double-to-float conversion emits FP32 for F64 input; output precision is
therefore 32, while the input codec and coded width remain F64/64.

Ordinary PCM goes through existing LAV/FFmpeg decoding and postprocessing.
Confirmed JOC still goes through existing OpenJOC admission. This patch does not
change rendering math, gain behavior, Raw SPDIF defaults, or the prior output
preroll/media-type notification fix. The existing PCM format enable/disable
setting still controls codec initialization. A non-JOC PCM or FLAC track should
show the stock-decoder state with no active JOC stream, inside the OpenJOC filter.

## Portable regression

Run:

```
python decoder/LAVAudio/tests/run_normal_pcm_input_tests.py
python decoder/LAVAudio/tests/run_gain_persistence_tests.py
```

Both runners accept `--compiler cl` in a Visual Studio developer shell. The PCM
runner compiles the production validator and unmodified production admission,
codec lookup and media-type initialization methods against deterministic
adapters. It tests side-by-side and stock compile modes, packed and container
precision, malformed/truncated input, format-disable behavior, the separate
Raw SPDIF gate, ordinary FLAC/E-AC-3 mapping, and arbitrary basic-PCM `cbSize`
(including `0xffff`) in an actual 18-byte allocation. The cbSize regression
fails against the prior production header and passes with this narrow fix. `--sanitize` enables address
and undefined-behavior sanitizers with compatible GCC/Clang hosts.

For a portable negative control, `--source-dir` may name baseline production
sources with the candidate validator header supplied for the test fixture. The
old methods must fail the first default ordinary-PCM admission assertion; the
stock-build assertions should still pass. This is method-level evidence, not
native COM or FFmpeg evidence.

## Native boundary and user verification

The capture-only `--pcm-track-admission` mode in
`OpenJocDirectShowNegotiationSmoke` uses a private graph and deterministic
fixtures. It is separate from the earlier `--preroll-format` regression; both
are required candidate gates. Consult the harness and workflow for the exact
invocation and runtime manifest checks. A successful native run establishes
only the tested private graph, splitter/input changes and capture sink. It does
not establish PotPlayer track-selection behavior or physical renderer support.

Do not play an unverified candidate audibly as a test. First require native
capture-only format and payload checks, then inspect the player's active filter
identity and transitions using a muted/capture-only setup. Candidate packages
are for testing and do not imply a stable release or automatic merge.
