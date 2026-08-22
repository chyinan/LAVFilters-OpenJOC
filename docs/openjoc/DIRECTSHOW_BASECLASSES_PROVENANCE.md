<!--
SPDX-FileCopyrightText: 2026 OpenJOC contributors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# DirectShow baseclasses and headerless DSUtilLite provenance

Release scope: openjoc-0.10.0  
LAV upstream revision: `fefb6987994ed56e4525e8a125f5fbb53707bc52`  
Evidence date: 2026-08-22

## Result

All 34 previously headerless compiled inputs are specifically classified: 31 DirectShow baseclasses compilation units, `DSMResourceBag.cpp`, `CSSauth.cpp`, and `CSSscramble.cpp`. No compiled input in this scope remains legally unclassifiable.

The two CSS units are GPL-3.0-only inputs. The remaining LAV-authored units are GPL-2.0-or-later. Because GPL-2.0-or-later permits selection of GPLv3 and MIT is GPL-compatible, the combined `LAVAudio.ax` candidate is classified for distribution under GPL-3.0-only. No GPL-2.0-only component was found.

## Microsoft DirectShow baseclasses

The exact project `common/baseclasses/baseclasses.vcxproj` compiles 31 C++ units. LAV Git history places every unit in the initial commit `61628549ed7f3b8ed4aa2596767659a5b698e4c8` (2010-08-01), followed by directory-only R100 moves. Each file retains the Microsoft DirectShow Base Classes copyright header.

The public Microsoft comparison source is `Samples/Win7Samples/multimedia/directshow/baseclasses` in [microsoft/Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples), revision `d59e5f1dc9c768615e4e1ab1f0f009e6a3ed747c`. The repository root [LICENSE](https://github.com/microsoft/Windows-classic-samples/blob/main/LICENSE) grants the MIT License and identifies Microsoft Corporation.

A filename-by-filename comparison found:

- 24 of the 31 LAV compilation units identical to the Microsoft sample after newline and whitespace normalization.
- Seven units changed in the LAV lineage: `amfilter.cpp`, `amvideo.cpp`, `dllsetup.cpp`, `winctrl.cpp`, `wxdebug.cpp`, `wxlist.cpp`, and `wxutil.cpp`.
- None of the 31 files has an OpenJOC downstream change.

The 24 normalized-identical files are classified MIT. The seven LAV-modified files contain MIT-origin Microsoft sample code plus LAV changes governed by GPL-2.0-or-later, and are classified `MIT AND GPL-2.0-or-later`. Confidence is high for the normalized-identical units and medium-high for the seven lineage-modified units.

Microsoft's current [DirectShow samples documentation](https://learn.microsoft.com/en-us/windows/win32/directshow/directshow-samples) identifies Base Classes as part of the DirectShow sample set and points developers to the Windows SDK sample lineage. This corroborates the file headers, official repository path, and Git comparison; it is not used to manufacture a license for a headerless file.

## DSMResourceBag

`common/DSUtilLite/DSMResourceBag.cpp` and its header were created in LAV commit `52d9d76c503fcea5e4a1431127fe2f916ba830db` on 2015-04-03 by Hendrik Leppkes, “Export coverart and attachments through IDSMResourceBag.” They are not copied from the compared MPC-HC or Microsoft Base Classes paths. The commit-era LAV project declaration, LAV `COPYING`, and the upstream project's GPLv2+ declaration cover this LAV-authored unit. It is classified GPL-2.0-or-later with high confidence. OpenJOC did not modify it.

## CSSauth and CSSscramble

LAV commit `bd86f1cf3f935fc92ece8bc0c5ff3a9b651d18dd` (2011-06-09, Hendrik Leppkes) added:

- `common/DSUtilLite/DeCSS/CSSauth.cpp`
- `common/DSUtilLite/DeCSS/CSSscramble.cpp`

The matching source-era MPC-HC tree contains `src/DeCSS/CSSauth.cpp` and `src/DeCSS/CSSscramble.cpp`. At MPC-HC revision `dcbf6bf36a37438f8eb25536aafe419c835fcd1c` (2011-06-09), the repository's `COPYING.txt` is GNU GPL version 3. LAV's import preserves the implementation and later LAV changes are formatting/maintenance. Each is therefore classified GPL-3.0-only with high confidence. OpenJOC did not modify either unit.

## Reproducible evidence commands

The evidence was produced from local read-only clones of the official repositories and the LAV worktree. The machine-readable per-input results are in `LAV_SOURCE_LICENSE_CENSUS.json`.

```text
git -C LAV log --diff-filter=A -- <exact-path>
git -C MPC-HC show dcbf6bf...:COPYING.txt
git -C Windows-classic-samples show d59e5f1d...:LICENSE
git diff --no-index --numstat -- <Microsoft-file> <LAV-file>
```

The local evidence clones and their `.git` object databases are audit inputs only and must not be included in the corresponding-source archive.
