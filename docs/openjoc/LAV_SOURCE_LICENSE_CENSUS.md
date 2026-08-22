<!--
SPDX-FileCopyrightText: 2026 OpenJOC contributors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# LAV source license census

Release: openjoc-0.10.0  
Census date: 2026-08-22  
Machine-readable census: `LAV_SOURCE_LICENSE_CENSUS.json`

## Scope and result

The census enumerates every `ClCompile Include` entry in the exact three projects linked into `LAVAudio.ax`:

| Project | Compiled units | Classification summary |
|---|---:|---|
| `decoder/LAVAudio/LAVAudio.vcxproj` | 12 | 12 GPL-2.0-or-later |
| `common/DSUtilLite/DSUtilLite.vcxproj` | 17 | 15 GPL-2.0-or-later; 2 GPL-3.0-only |
| `common/baseclasses/baseclasses.vcxproj` | 31 | 24 MIT; 7 MIT AND GPL-2.0-or-later |
| **Total** | **60** | **0 unresolved** |

The effective combined distribution license is GPL-3.0-only because the two CSS compilation units are GPL-3.0-only and the GPL-2.0-or-later units may be distributed under GPLv3. The MIT inputs are compatible. No GPL-2.0-only compiled component and no known license incompatibility were found.

## Headerless closure

| Source | Origin and revision | Result | Confidence |
|---|---|---|---|
| `common/DSUtilLite/DSMResourceBag.cpp` | LAV creation commit `52d9d76...` | GPL-2.0-or-later | High |
| `common/DSUtilLite/DeCSS/CSSauth.cpp` | MPC-HC GPLv3 tree `dcbf6bf...`; LAV import `bd86f1c...` | GPL-3.0-only | High |
| `common/DSUtilLite/DeCSS/CSSscramble.cpp` | MPC-HC GPLv3 tree `dcbf6bf...`; LAV import `bd86f1c...` | GPL-3.0-only | High |
| 24 Microsoft Base Classes units | Microsoft sample `d59e5f1...`; LAV import `6162854...`; normalized-identical | MIT | High |
| 7 modified Microsoft Base Classes units | Same Microsoft/LAV lineage, with LAV changes | MIT AND GPL-2.0-or-later | Medium-high |

See `DIRECTSHOW_BASECLASSES_PROVENANCE.md` for the exact evidence chain. The JSON census contains one record for each of all 60 compiled units, including origin, revision, license evidence, LAV change state, OpenJOC change state, classification, and confidence.

## OpenJOC-created LAV files

All seven new integration/test/smoke files carry:

- `SPDX-FileCopyrightText: 2026 OpenJOC contributors`
- `SPDX-License-Identifier: GPL-2.0-or-later`

They are `LAVAudioIdentitySmoke.cpp`, `OpenJocAdmission.cpp/.h`, `OpenJocAdmissionTests.cpp`, `OpenJocDecoder.cpp/.h`, and `OpenJocDecoderSmoke.cpp`.

## Modified upstream census

All seven OpenJOC-modified upstream files preserve their original notices and contain an OpenJOC downstream modification notice naming `openjoc-0.10.0` and 2026-08-22:

- `common/includes/common_defines.h`
- `decoder/LAVAudio/AudioSettingsProp.cpp`
- `decoder/LAVAudio/LAVAudio.cpp`
- `decoder/LAVAudio/LAVAudio.h`
- `decoder/LAVAudio/LAVAudio.vcxproj`
- `decoder/LAVAudio/dllmain.cpp`
- `include/LAVAudioSettings.h`

Their machine-readable records appear under `modified_upstream_files` in the JSON census.

## Boundary

The census does not infer a license solely from the absence of a header. Every headerless compiled unit is tied to a named source tree, revision, project-level license evidence, file ancestry, and comparison result. Test/smoke files that are not linked into `LAVAudio.ax` are covered separately by their explicit SPDX headers.
