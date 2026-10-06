#!/usr/bin/env python3
"""Compile unmodified production methods against deterministic lock/registry adapters.

This is a logic regression test, not a native DirectShow/registry integration test.
Run with Python 3 and g++/clang++, or --compiler cl in a VS developer shell.
--source accepts an older LAVAudio.cpp for red-before verification.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=HERE.parent / 'LAVAudio.cpp')
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')


def method(name):
    start = source.index('HRESULT CLAVAudio::' + name + '(')
    # Methods end with a top-level closing brace (nested braces are indented).
    end = source.index('\n}', start) + 2
    return source[start:end]


methods = '\n\n'.join(method(name) for name in (
    'LoadOpenJocOutputGainSettings', 'SaveOpenJocOutputGainSettings',
    'SetRuntimeConfig', 'GetOutputGain', 'SetOutputGain'))
with tempfile.TemporaryDirectory(prefix='lav-gain-persistence-') as temp:
    work = Path(temp)
    (work / 'ProductionMethods.inc').write_text(methods, encoding='utf-8')
    fixture = HERE / 'GainPersistenceTests.cpp'
    binary = work / 'gain-persistence-tests.exe'
    if Path(args.compiler).stem.lower() == 'cl':
        command = [args.compiler, '/nologo', '/std:c++17', '/EHsc', '/W4',
                   '/I' + str(work), str(fixture), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-pthread', '-I', str(work), str(fixture), '-o', str(binary)]
    subprocess.run(command, cwd=work, check=True, timeout=120)
    subprocess.run([str(binary)], cwd=work, check=True, timeout=30)
