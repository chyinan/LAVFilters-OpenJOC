#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenJOC contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile unmodified output queue/status/delivery methods against COM/AV adapters.

This is a source-seam regression, not a native DirectShow/FFmpeg test.
--source accepts a pre-fix LAVAudio.cpp for the failing status control.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=HERE.parent / 'LAVAudio.cpp')
parser.add_argument('--source-dir', type=Path, default=HERE.parent,
                    help='directory containing OpenJocStrictOutput/Negotiation.cpp')
parser.add_argument('--compiler', default='g++')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--cxxflag', action='append', default=[],
                    help='extra compiler flag; use --cxxflag=-flag')
args = parser.parse_args()


def extract(source, signature):
    start = source.index(signature)
    # Every extracted function has its closing brace at column zero; nested
    # braces and initializer/lambda braces are indented. No body reconstruction.
    end = source.index('\n}', start) + 2
    return source[start:end]


audio = args.source.read_text(encoding='utf-8')
strict = (args.source_dir / 'OpenJocStrictOutput.cpp').read_text(encoding='utf-8')
negotiation = (args.source_dir / 'OpenJocStrictNegotiation.cpp').read_text(encoding='utf-8')
parts = []
for signature in (
    'bool IsCanonicalContract(', 'bool BuildLAVOpenJocStrictMediaType(',
    'bool IsExactLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,\n'
    '                                      const LAVOpenJocStrictMediaType &candidate)',
    'bool IsExactLAVOpenJocStrictMediaType(const LAVOpenJocOutputContract &contract,\n'
    '                                      const AM_MEDIA_TYPE &candidate)',
    'bool CheckedLAVOpenJocSampleAdd(', 'bool CheckedLAVOpenJocPcmByteCount(',
    'bool CheckedLAVOpenJocLongNarrow(', 'bool AreLAVOpenJocBufferContractsCompatible(',
    'bool ValidateLAVOpenJocStrictBuffer(', 'HRESULT NormalizeLAVOpenJocQueryAcceptResult(',
    'HRESULT NormalizeLAVOpenJocEndOfStreamStep(', 'HRESULT ValidateLAVOpenJocDeliverySample(',
):
    parts.append(extract(strict, signature))
for signature in ('HRESULT DeliverLAVOpenJocStrictMediaType(',
                  'HRESULT ExecuteLAVOpenJocQueueTransaction('):
    parts.append(extract(negotiation, signature))
for signature in (
    'HRESULT CLAVAudio::QueueOutput(', 'HRESULT CLAVAudio::FlushOutput(',
    'HRESULT CLAVAudio::FlushOutputLocked(', 'HRESULT CLAVAudio::GetOutputDetails(',
    'static HRESULT CreateOpenJocStrictDirectShowMediaType(',
    'HRESULT CLAVAudio::Deliver(', 'HRESULT CLAVAudio::PrepareOpenJocDelivery(',
    'HRESULT CLAVAudio::CompleteOpenJocDelivery(', 'HRESULT CLAVAudio::EndOfStream(',
):
    parts.append(extract(audio, signature))
for part in parts:
    print('SOURCE', part.split('\n', 1)[0], hashlib.sha256(part.encode()).hexdigest(), flush=True)
methods = '\n\n'.join(parts)
cl = Path(args.compiler).stem.lower() == 'cl'
if cl and args.sanitize:
    parser.error('--sanitize is supported by GCC/Clang, not this cl runner')
if not cl:
    # The only source-text portability adaptation is Microsoft's i64 suffix.
    methods = methods.replace('10000i64', '10000LL')
with tempfile.TemporaryDirectory(prefix='lav-output-status-') as temp:
    work = Path(temp)
    (work / 'OutputStatusProductionMethods.inc').write_text(methods, encoding='utf-8')
    binary = work / 'output-status-fallback-tests.exe'
    fixture = HERE / 'OutputStatusFallbackTests.cpp'
    if cl:
        command = [args.compiler, '/nologo', '/std:c++17', '/EHsc', '/W4',
                   '/I' + str(work), str(fixture), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(work), str(fixture), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command += args.cxxflag
    subprocess.run(command, cwd=work, check=True, timeout=120)
    result = subprocess.run([str(binary)], cwd=work, timeout=30)
    raise SystemExit(result.returncode)
