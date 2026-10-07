<!-- SPDX-FileCopyrightText: 2026 OpenJOC contributors -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# PCM / JOC same-instance capture regression

The native `OpenJocDirectShowNegotiationSmoke.exe` mode is:

```
OpenJocDirectShowNegotiationSmoke.exe --pcm-track-admission <runtime-dir> <runtime-manifest> <fixture-dir>
```

This mode loads the manifest-verified private side-by-side Audio and Splitter
modules. It does not register filters, modify machine or user registry settings,
enumerate/connect an audio renderer, or produce audible output. All Audio and
Splitter settings changes are runtime-only. It requires the side-by-side
inspection and diagnostics interfaces.

## Prepare reproducible fixtures

From the LAV repository root, with Python 3 and an existing, valid raw JOC
E-AC-3 fixture that produces nonzero output:

```
python decoder/LAVAudio/tests/prepare_pcm_track_fixtures.py --output-dir <fixture-dir> --joc-fixture <joc.lifecycle.ec3>
```

The generator uses only the Python standard library. It writes 8192-frame,
48 kHz (plus packed-24 and FP32 96 kHz), channel-distinct, low-level integer/float fixtures, byte-for-byte stock
output oracles (including nonzero low precision bits in both 24-bit cases),
a tiny valid FLAC using VERBATIM subframes with CRC8/CRC16 and
STREAMINFO MD5, and a SHA-256/size manifest. It copies the supplied JOC fixture;
it does not generate or redistribute that content. `--ffmpeg <executable>` is
optional and independently verifies the generated FLAC, packed 24-bit PCM, and
F64-to-FP32 payload oracles. FFmpeg is not required on the Windows capture host.

## Native assertions

Each of eight cases creates its own private Audio instance, one source and one
capture sink. The Audio instance is retained across that case's five phases;
it is not shared across the eight cases:

1. 16-bit integer stereo WAV
2. 32-bit IEEE float stereo WAV
3. 64-bit IEEE float stereo WAV, with preferred enumerated output metadata and
   delivered stock FP32 output both checked
4. Packed 24-bit integer, 5.1 extensible WAV
5. Explicit 24-valid-bit / 32-bit-container, 5.1 extensible input
6. FLAC stereo control using the same lossless samples as case 1
7. Packed 24-bit integer, 5.1 extensible WAV at 96 kHz
8. 32-bit IEEE float stereo WAV at 96 kHz

The sequence for every case is stock → JOC → stock → JOC → stock. Only the
stopped source is replaced between phases; the same Audio instance, output pin,
and downstream capture connection are retained. Normal cases exercise actual
splitter-produced media types and packets. The harness checks input subtype,
format tag/subformat, channel count/mask, rate, valid bits, container bits,
block alignment, byte rate and format length where applicable. The first six
cases retain their original 48 kHz coverage. Cases 7 and 8 each exercise
96 kHz PCM → 48 kHz JOC → 96 kHz PCM → 48 kHz JOC → 96 kHz PCM.
The stock output oracle uses `WAVEFORMATEXTENSIBLE` above 48 kHz, including
stereo FP32, matching LAV's existing `CreateMediaType` rule. The first high-rate
FP32 run exposed an incorrect basic-header expectation in the harness; the
strict sink rejected the real extensible FP32 type and LAV attempted its stock
PCM16 fallback. Correcting that expected representation keeps the exact FP32
payload, peer-type, sample-type, timing and gain assertions intact. No raw-SPDIF
input is allowed. Every PCM/FLAC output byte must match its fixture oracle.

The first JOC pass uses 0 dB and the second +6 dB, checked sample-by-sample
against independent scalar math. Every stock phase retains +6 dB in settings
while still producing the unchanged stock payload. Gain is set only for the first
three phases, then read back without resetting it for the second JOC and final
stock phase; it is also checked after each Stop. Both JOC passes must report
OpenJOC admission and nonzero classifier/stream counters. Every stock phase,
including FLAC, must report undecided/no active JOC state, zero counters, no
warning or failure details, and `S_FALSE` from the live inspection JSON API.
A non-JOC FLAC track showing no active JOC stream is the expected result.

Every changed output format must be attached to the first delivered sample of
that phase and exactly match the receiving pin. The harness also checks exact
output metadata, allocator/sample contracts, contiguous nonnegative timestamps
and per-sample durations matching the current format's rate (within one 100 ns
rounding tick),
one fresh EOS per phase, no graph errors, preserved requested policy/gain, and
runtime/fixture identity after capture. It uses the strict 7.1.4 FP32 contract
for JOC, making the stock/JOC transitions observable.

### Why case 5 uses explicit input injection

Some FFmpeg WAV demuxers reinterpret 24 valid bits in a 32-bit container as
CoolEdit float or repack the input before LAV sees it. Case 5 therefore loads a
normal packed-24 source solely to establish the graph, leaves that source
stopped, and synchronously delivers actual 32-bit-container samples carrying
an explicit extensible type to the real Audio input pin. It verifies the input
pin adopted that exact type, the source stayed stopped, and output was the exact
packed-24 oracle. This exercises the decoder's sample-attached input-type path
without mistaking splitter normalization for decoder evidence. JOC phases still
use the real splitter and keep the same downstream connection.

## Results and failure markers

A fully successful native run exits 0 and prints `PCM_TRACK_COMPLETE`. It also
prints per-phase metadata/counts and `PCM_TRACK_CASE_COMPLETE` for all eight cases.
Failure exits 1 and prints `PCM_TRACK_UNVERIFIED`. Reverting only the PCM admission
fix should reject the first s16 input with `PCM_TRACK_INPUT_REJECTED` before any
successful PCM capture. That negative control must not be reported as a Windows
pass merely because the expected text appears: require nonzero exit, the input
rejection marker, and absence of the final success marker.

The portable checks are:

```
python decoder/LAVAudio/tests/run_pcm_track_harness_tests.py
python decoder/LAVAudio/tests/run_pcm_track_harness_tests.py --ffmpeg ffmpeg
```

Use `--compiler cl` in a Visual Studio developer shell, or the default `g++` on
Linux. They compile the native harness's unmodified metadata/gain oracle
functions against small Windows-type adapters, check positive and corrupted
oracles, deterministic fixtures, and source-route invariants. These checks do
not compile the complete native harness or exercise DirectShow/COM/FFmpeg/LAV.

Keep the older `--preroll-format` native gate as a separate regression. Neither
capture mode proves host filter enumeration/selection, PotPlayer's actual track-selection graph, a user's saved
filter configuration, a specific renderer, or physical audio output. Windows
native compilation and capture execution must be recorded separately from the
portable checks; no local Windows result is implied by adding this test.

The original user-reported private `MEDIASUBTYPE_FFMPEG_AUDIO` route is resolved:
OpenJOC-first filter priority restored PCM ↔ JOC switching, as confirmed by the
user. Ordinary-PCM admission is an independent feature. These direct-load tests
do not replace that user confirmation or establish behavior in another host.

## Capture receiver media-type commit regression

The first Windows run passed ordinary s16 capture and delivered 129 JOC samples,
but correctly failed the exact peer-type assertion. Its capture sink recorded
the delivered FP32 type separately while its actual DirectShow input pin still
reported PCM16. `CBaseInputPin::Receive` validates attached media types but does
not call `SetMediaType`; the derived receiver must commit an accepted change.

`StrictCaptureInputPin::Receive` now commits a changed attached type only after
both the base receive and sample capture return exactly `S_OK`. Base/capture
failure or `S_FALSE` leaves the pin unchanged; an absent or identical attached
type does not produce a redundant commit. Commit failures are propagated. The
exact sender/receiver connection assertions remain in place. `PCM_TRACK_WITNESS`
reports every gate independently, including the two pin types and diagnostics
statuses, so an earlier failure no longer leaves misleading unread counters.

The portable runner extracts that unmodified native receive method and tests
successful changed-type commit, no-type/same-type no-op, base/capture failure
and `S_FALSE`, commit failure propagation, and a return transition. `--source`
can name the pre-fix harness for a negative control; it must fail the first
successful-delivery receiver-type assertion. This validates the harness model,
not Windows execution. The native PCM and preroll gates must still be rerun.
