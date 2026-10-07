#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenJOC contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test the native capture harness oracles and fixture generation, not Windows COM."""
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default='g++')
parser.add_argument('--ffmpeg', help='Optional independent verification of lossless fixtures')
args = parser.parse_args()
source = (HERE.parent / 'OpenJocDirectShowNegotiationSmoke.cpp').read_text(encoding='utf-8')

def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

start = source.index('struct PcmTrackCase\n')
case = source[start:source.index('\n};', start) + 3]
methods = case + '\n' + '\n'.join(function(signature) for signature in (
    'CMediaType BuildTrackPcmType(', 'bool TrackInputMetadataMatches(', 'bool VerifyTrackGain('))
# Guard this mode's route rather than the entire multipurpose executable.
lane = source[source.index('HRESULT RunPcmTrackSequence('):source.index('HRESULT RunOpenJocLifecycleMatrix(')]
for forbidden in ('VolatileCurrentUserOverride', 'RegSetValue', 'RegCreateKey', 'RenderFile(',
                  'CreateNativeRenderer', 'CLSID_AudioRender', 'SetDefaultAudioEndpoint'):
    assert forbidden not in lane, forbidden
for required in ('SetAllowRawSPDIFInput(FALSE)', 'Codec_PCM, TRUE', 'Codec_FLAC, TRUE',
                 'samples[first].has_attached_type', 'bytes != oracle', 'VerifyTrackGain(unity_joc, bytes)',
                 'CopyOpenJocLiveInspectionJson(nullptr, 0, &required) != S_FALSE',
                 'GraphContainsExactly(graph.get(), 3)', 'test.inject_24_in_32',
                 'L"f64"', 'L"flac-control"', 'audio_output->EnumMediaTypes(output_types.put())',
                 'ExactMediaTypeEqual(*preferred, stock_output)', 'if (phase < 3)'):
    assert required in lane, required
spec = importlib.util.spec_from_file_location('fixtures', HERE / 'prepare_pcm_track_fixtures.py')
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
with tempfile.TemporaryDirectory(prefix='lav-pcm-track-harness-') as temporary:
    work = Path(temporary)
    (work / 'PcmTrackHarnessMethods.inc').write_text(methods, encoding='utf-8')
    binary = work / 'pcm-track-oracles.exe'
    if Path(args.compiler).stem.lower() == 'cl':
        command = [args.compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/I' + str(work),
                   str(HERE / 'PcmTrackHarnessTests.cpp'), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(work),
                   str(HERE / 'PcmTrackHarnessTests.cpp'), '-o', str(binary)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(binary)], cwd=work, check=True)
    first = fixtures.prepare(work / 'first', ffmpeg=args.ffmpeg)
    second = fixtures.prepare(work / 'second')
    assert first == second, 'Fixture generation must be deterministic'
    padded = (work / 'first/pcm.s24in32.input.pcm').read_bytes()
    oracle = (work / 'first/pcm.s24in32.expected.pcm').read_bytes()
    assert len(padded) == fixtures.FRAMES * 6 * 4 and len(oracle) == fixtures.FRAMES * 6 * 3
    assert b''.join(padded[offset+1:offset+4] for offset in range(0, len(padded), 4)) == oracle
    assert all(padded[offset] == 0 for offset in range(0, len(padded), 4))
    assert any(oracle[offset] != 0 for offset in range(0, len(oracle), 3))
    truncated16 = b''.join(b'\0' + oracle[offset+1:offset+3] for offset in range(0, len(oracle), 3))
    assert truncated16 != oracle, 'Packed24 must detect discarded low precision bits'
    wav = (work / 'first/pcm.s24in32.wav').read_bytes()
    assert struct.unpack_from('<H', wav, 34)[0] == 32
    assert struct.unpack_from('<H', wav, 38)[0] == 24
    assert struct.unpack_from('<I', wav, 40)[0] == 0x3f
    print('PCM_TRACK_FIXTURE_ORACLES_PASS files=' + str(len(first)))
