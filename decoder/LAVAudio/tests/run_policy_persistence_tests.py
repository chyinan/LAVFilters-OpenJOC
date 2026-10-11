#!/usr/bin/env python3
"""Portable production-method ordering regression; no COM/Windows coverage."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=HERE.parent / 'LAVAudio.cpp')
parser.add_argument('--compiler', default='g++')
parser.add_argument('--cxxflag', action='append', default=[])
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')
header = (HERE.parents[2] / 'include/LAVOpenJocSettings.h').read_text(encoding='utf-8')
def method(name):
    start = source.index('HRESULT CLAVAudio::' + name + '(')
    end = source.index('\n}', start) + 2
    text = source[start:end]
    print(name, hashlib.sha256(text.encode()).hexdigest(), flush=True)
    return text
methods = '\n\n'.join(method(name) for name in (
    'GetOutputPolicy', 'SetOutputPolicy', 'ConfigureOpenJocOutputPolicy',
    'SaveOpenJocOutputPolicySettings', 'LoadOpenJocOutputPolicySettings',
    'SetBinauralConfiguration', 'SetRuntimeConfig'))
definitions = re.search(r'inline constexpr std::uint32_t LAV_OPENJOC_OUTPUT_POLICY_SCHEMA_VERSION = .*?;', header)[0]
definitions += '\n' + re.search(r'enum class LAVOpenJocOutputPolicy[^}]+};', header)[0]
with tempfile.TemporaryDirectory(prefix='lav-policy-persistence-') as temp:
    work = Path(temp)
    (work / 'PolicyProductionMethods.inc').write_text(methods, encoding='utf-8')
    (work / 'PolicyDefinitions.inc').write_text(definitions, encoding='utf-8')
    fixture = HERE / 'PolicyPersistenceTests.cpp'
    binary = work / 'policy-persistence-tests.exe'
    if Path(args.compiler).stem.lower() == 'cl':
        if args.sanitize:
            parser.error('--sanitize requires GCC or Clang')
        command = [args.compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/I' + str(work), str(fixture), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(work), str(fixture), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + args.cxxflag, cwd=work, check=True, timeout=120)
    subprocess.run([str(binary)], cwd=work, check=True, timeout=30)
