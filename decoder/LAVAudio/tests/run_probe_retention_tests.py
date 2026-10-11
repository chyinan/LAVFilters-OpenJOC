#!/usr/bin/env python3
"""Compile actual admission logic/Process with a byte-capture decoder adapter.

Supply the unchanged public OpenJOC crates/openjoc-wasm/testdata/joc.lifecycle.ec3
fixture. This does not decode or classify compressed data.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=HERE.parent / 'OpenJocDecoder.cpp')
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--compiler', default='g++')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')
start = source.index('LAVOpenJocProcessResult LAVOpenJocDecoder::Process(')
method = source[start:source.index('\n}', start) + 2]
print('Process SHA256', hashlib.sha256(method.encode()).hexdigest(), flush=True)
print('fixture SHA256', hashlib.sha256(args.fixture.read_bytes()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix='lav-probe-retention-') as temp:
    work = Path(temp)
    (work / 'ProbeProcess.inc').write_text(method, encoding='utf-8')
    binary = work / 'probe-retention-tests.exe'
    command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2',
               '-pthread', '-I', str(work), '-I', str(HERE.parent),
               str(HERE / 'ProbeRetentionTests.cpp'), str(HERE.parent / 'OpenJocAdmission.cpp'),
               '-o', str(binary)]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, cwd=work, check=True, timeout=120)
    subprocess.run([str(binary), str(args.fixture.resolve())], cwd=work, check=True, timeout=30)
