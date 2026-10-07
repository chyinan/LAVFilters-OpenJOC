#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenJOC contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production PCM validator and unmodified admission/mapping/init methods.

The adapters mock COM and ffmpeg_init; run the native capture-only regression
separately for actual decoder, splitter, payload and transition evidence.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default='g++')
parser.add_argument('--source-dir', type=Path, default=HERE.parent)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
audio = (args.source_dir / 'LAVAudio.cpp').read_text(encoding='utf-8')
media = (args.source_dir / 'Media.cpp').read_text(encoding='utf-8')


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


# Keep the real conditional registration entries, not a test-side copy.
registration = media.split('const AMOVIESETUP_MEDIATYPE CLAVAudio::sudPinTypesIn[] = {', 1)[1].split('  // DVD Types', 1)[0]
methods = '\n\n'.join([
    'const AMOVIESETUP_MEDIATYPE CLAVAudio::sudPinTypesIn[] = {' + registration +
    '\n{ &MEDIATYPE_Audio, &MEDIASUBTYPE_FLAC },\n{ &MEDIATYPE_Audio, &MEDIASUBTYPE_DOLBY_DDPLUS }};\n' +
    'const UINT CLAVAudio::sudPinTypesInCount = countof(CLAVAudio::sudPinTypesIn);',
    method(media, 'AVCodecID FindCodecId('),
    method(audio, 'HRESULT CLAVAudio::CheckInputType('),
    method(audio, 'HRESULT CLAVAudio::SetMediaType('),
])
with tempfile.TemporaryDirectory(prefix='lav-normal-pcm-') as temp:
    work = Path(temp)
    (work / 'NormalPcmProductionMethods.inc').write_text(methods, encoding='utf-8')
    for side_by_side in (False, True):
        binary = work / ('normal-pcm-sbs.exe' if side_by_side else 'normal-pcm-stock.exe')
        if Path(args.compiler).stem.lower() == 'cl':
            command = [args.compiler, '/nologo', '/std:c++17', '/EHsc', '/W4',
                       '/I' + str(work), '/I' + str(args.source_dir.resolve()),
                       str(HERE / 'NormalPcmInputTests.cpp'), '/Fe:' + str(binary)]
            if side_by_side:
                command.append('/DLAV_OPENJOC_SIDE_BY_SIDE')
        else:
            command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-Wno-sign-compare', '-I', str(work), '-I', str(args.source_dir.resolve()),
                       str(HERE / 'NormalPcmInputTests.cpp'), '-o', str(binary)]
            if side_by_side:
                command.append('-DLAV_OPENJOC_SIDE_BY_SIDE')
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=work, check=True, timeout=120)
        subprocess.run([str(binary)], cwd=work, check=True, timeout=30)
