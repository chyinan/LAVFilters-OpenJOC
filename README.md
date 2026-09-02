# LAV Filters — OpenJOC integration

This repository is a downstream fork of [Nevcairiel/LAVFilters](https://github.com/Nevcairiel/LAVFilters).

- Upstream base: LAV Filters 0.83 (`fefb6987994ed56e4525e8a125f5fbb53707bc52`)
- Purpose: optional OpenJOC JOC decoding in the LAV Audio Decoder
- Ordinary E-AC-3: stock LAV/FFmpeg decoding
- Ordinary AC-3: stock LAV/FFmpeg decoding
- Confirmed JOC: OpenJOC admission and decoding, including the standards-defined
  legacy AC-3 core + dependent E-AC-3 D0 carriage
- Malformed or unsupported streams remain fail-closed; no user toggle is required
- E-AC-3 passthrough: existing LAV bitstream path takes precedence
- OpenJOC DirectShow output in this integration: explicit fixed 48 kHz IEEE
  float PCM policies for Stereo (Speakers), Binaural (Headphones), 5.1, 7.1,
  5.1.2, 5.1.4, 7.1.2, and 7.1.4
- Negotiation: one exact semantic `WAVEFORMATEXTENSIBLE` proposal per selected
  policy, with no fallback mask or alternate proposal
- Automatic layout selection: `AUTO_NOT_RELIABLE`; Stereo is the default
- Scope: no endpoint-name layout inference, Bass Management, physical-subwoofer
  routing, or physical multichannel hardware verification
- OpenJOC project: [chyinan/OpenJOC](https://github.com/chyinan/OpenJOC)
- Downstream integration tag: `openjoc-0.16.0`

This downstream project is not endorsed by Nevcairiel, FFmpeg, PotPlayer,
Dolby, Microsoft, or SADIE. See `docs/openjoc/` and the upstream `COPYING`
file for provenance and applicable license information.

---

## Playback output UX

**Stereo (Speakers)** is conventional two-channel speaker playback without
HRTF processing. **Binaural (Headphones)** renders the selected OpenJOC
virtual speaker field through the built-in SADIE II D1 KU100 HRTF or one
user-selected local SOFA dataset and emits two-channel headphone PCM. Both
policies use ordinary two-channel WAVEFORMATEXTENSIBLE output, but their
rendering semantics are different.

The output policy is an explicit user choice. The filter does not detect
headphones, speakers, endpoints, Bluetooth devices, HDMI capabilities, or
USB DACs. Speaker layouts must match a supported downstream layout. The
built-in HRTF is generic and uses the embedded offline resource documented in
the OpenJOC third-party notices. A Custom SOFA selection is passed to the
existing strict OpenJOC SOFA loader; the file must remain readable and within
that loader's supported SimpleFreeFieldHRIR subset. Invalid or missing files
are reported as Binaural HRTF configuration errors and do not silently fall
back to the built-in HRTF. This integration does not add head tracking or
personalized HRTF controls.

The Binaural page also selects the virtual speaker layout: **7.1.4
(Recommended)** or **9.1.6 (Experimental)**. Both layouts use the existing
OpenJOC virtual-speaker stage followed by the same SOFA/HRTF backend; final
output remains two-channel binaural PCM. 9.1.6 is not claimed to be better or
reference quality, and its additional virtual feeds may cost more CPU.
The canonical 9.1.6 intermediate order is `FL, FR, FC, LFE, Lb, Rb, Ls, Rs,
Lw, Rw, Ltf, Rtf, Ltm, Rtm, Ltr, Rtr`; it is never exposed as 16-channel
DirectShow output.

The Status page distinguishes three playback states: OpenJOC, Stock decoder,
and Stock decoder (OpenJOC fallback). Ordinary AC-3/E-AC-3 remains normal
stock decoding with no warning. A fallback warning retains its structured
reason and bounded detail for the current stream; it is cleared when a new
stream is positively classified. A downstream output-layout rejection is
shown as an OpenJOC output error and is not relabeled as file corruption or
silently switched to stock decoding after promotion.

Channel Output meters are diagnostic-only: they read the final selected PCM
output (including the final two-channel Binaural signal) through the same
statistics lifecycle for Stock and OpenJOC. Status snapshots use non-blocking
state publication, so an in-flight OpenJOC render cannot stall the meter UI;
the last textual snapshot is retained until the next readable tick.

---

LAV Filters - ffmpeg based DirectShow Splitter and Decoders
=============================

LAV Filters are a set of DirectShow filters based on the libavformat and libavcodec libraries
from the ffmpeg project, which will allow you to play virtually any format in a DirectShow player.

The filters are still under development, so not every feature is finished, or every format supported.

Install
-----------------------------
- Unpack
- Register (install_*.bat files)
	Registering requires administrative rights, and an elevated shell ("Run as Administrator")

Using it
-----------------------------
By default, the splitter will register for all media formats that have been
tested and found working at least partially.
This currently includes (but is not limited to)
	MKV/WebM, AVI, MP4/MOV, TS/M2TS/MPG, FLV, OGG, BluRay (.bdmv and .mpls)

However, some other splitters register in a "bad" way and force all players
to use them. The Haali Media Splitter is one of those, and to give priority
to the LAVFSplitter you have to either uninstall Haali or rename its .ax file
at least temporarily.

The Audio and Video Decoder will register with relatively high merit, which should make
it the preferred decoder by default. Most players offer a way to choose the preferred
decoder however.

Automatic Stream Selection
-----------------------------
LAV Splitter offers different ways to pre-select streams when opening a file.
The selection of video streams is not configurable, and LAV Splitter will quite simply
pick the one with the best quality.

Audio stream selection offers some flexibility, specifically you can configure your preferred languages.
The language configuration is straightforward. Just enter a list of 3-letter language codes (ISO 639-2),
separated by comma or space.

For example: `eng ger fre`. This would try to select a stream matching one of these languages,
in the order you specified them. First, check if an English track is present, and only if not,
go to German, and after that, go to French.

If multiple audio tracks match one language, the choice is based on the quality. The primary attribute here
is the number of channels, and after that is the codec used. PCM and lossless codecs have a higher priority
than lossy codecs.

Subtitle selection offers the most flexibility.
There are 4 distinct modes of subtitle selection.

#### No Subtitles
This mode is simple, by default subtitles will be off.

#### Only Forced Subtitles
This mode will only pre-select subtitles flagged with the "forced" flag. It'll also obey the language preferences, of course.

#### Default
The default mode will select subtitles matching your language preference. If there is no match, or you didn't configure
languages, no subtitles will be activated. In addition, subtitles flagged "default" or "forced" will always be used.

#### Advanced
The advanced mode lets you write your own combinations of rules with a special syntax. It also allows selecting subtitles
based on the audio language of the file.

The base syntax is simple, it always requires a pair of audio and subtitle language, separated by a colon, for example: `eng:ger`
In this example, LAV Splitter would select German subtitles if English audio was found.

Instead of language codes, the advanced mode supports two special cases: `*` and `off`.
When you specify `*` for a language code, it'll match everything. For example `*:eng`  will activate English subtitles, independent
of the audio language. The reverse is also possible: `eng:*` will activate any subtitles when the audio is English.

The "off" flag is only valid for the subtitle language, and it instructs LAV Splitter to turn the subtitles off.
So "eng:off" means that when the audio is English, the subtitles will be deactivated.

Additionally to the syntax above, the following flags can be appended to the subtitle token separated by a pipe symbol (`|`):
 - `d` for default subtitles
 - `f` for forced subtitles
 - `h` for hearing impaired
 - `n` for normal streams (not default, forced, or impaired).

In addition, you can also check for the absence of flags by preceding the flags with a `!`.
The advanced rules can be combined into a complete logic for subtitle selection by just appending them, separated with a comma or a space.
The rules will always be parsed from left to right, the first match taking precedence.

Finally, the rules can match the name of a stream, with some limitations. Only single words can be matched, as spaces are a separator for the next token.
A text match can be added to the end of the token with a `@` sign.

Example: (basic flag usage)
- `*:*|f`
  - On any audio language, load any subtitles that are flagged forced.

Example: (basic ruleset)
- `eng:eng|f eng:ger|f eng:off *:eng *:ger`
  - If the audio is English, load an English or a German forced subtitle track, otherwise, turn subtitles off.
  - If the audio is not English, load English or German subtitles.

Example: (flag usage with negation)
- `jpn:ger|d!f`
  - In the Japanese language, load German subtitles that have the default-flag but not together with forced-flag.
  - This is useful when you have files where the default and forced flags are set together.

Example: (advanced ruleset for files with multiple audio and subtitle-tracks)
- `jpn:ger|d!f  jpn:ger|!f  jpn:ger  ger:ger|f  ger:eng|f  ger:*|f`
  - On Japanese audio, try to load German full subs (default but not forced), then unforced, and at last any german subs if there are no unforced subs.
  - On German audio load only forced subs in the following order: German, English, any.

Example: (text match)
- `*:eng@Forced`
  - On any audio, select english subtitle streams with "Forced" in the stream title.

Blu-ray Support
-----------------------------
To play a BluRay, simply open the index.bdmv file in the BDMV folder on the BluRay disc.
LAV Splitter will then automatically detect the longest track on the disc (usually the main movie),
and start playing.
Alternatively, you can also open a playlist file (*.mpls, located in BDMV/PLAYLIST), and LAV Splitter
will then play that specific title.

In future versions, you'll be able to choose the title from within the player, as well.

Compiling
-----------------------------
Compiling is pretty straightforward using VS2022 (included project files).
Older versions of Visual Studio are not officially supported, but may still work.

It does, however, require that you build your own FFmpeg.
FFmpeg is included as part of the repository in a submodule, pointing to a custom
fork of FFmpeg with various patches for improved compatibility with LAV and DirectShow.

A script is provided to compile FFmpeg using MSYS2 with MINGW/GCC or MSVC.

The custom fork of FFmpeg can be found here:
https://gitea.1f0.de/LAV/FFmpeg

libbluray is compiled with the MSVC project files, however, as with FFmpeg a custom
version is used, which is also linked as a Git submodule.

You can get find the custom version of libbluray here:
https://gitea.1f0.de/LAV/libbluray

Feedback
-----------------------------
GitHub Project: https://github.com/Nevcairiel/LAVFilters
Doom9: https://forum.doom9.org/showthread.php?t=156191
