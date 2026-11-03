#!/usr/bin/env python3
"""Exercise native CPUCP FAST hints, policy ownership and mailbox lifetime."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = 'drivers/soc/qcom/cpucp_fast.c'


def function(source, name):
    match = re.search(r'(?m)^static [^\n;{}]*\b' + name + r'\([^;{}]*\)\n\{', source)
    if not match:
        raise ValueError('Missing function: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def verify(staged):
    path = Path(staged) / SOURCE
    source = path.read_text()
    definitions = source[source.index('#define FAST_MBOX_CPUMASK'):source.index('static void qcom_cpucp_fast_rx')]
    funcs = '\n'.join(function(source, name) for name in (
        'qcom_cpucp_fast_rx', 'qcom_cpucp_fast_stop',
        'qcom_cpucp_fast_probe', 'qcom_cpucp_fast_shutdown'))
    harness = (HERE / 'tests/cpucp_fast_harness.c').read_text()
    harness = harness.replace('/* INSERT_DECLARATIONS */', definitions)
    harness = harness.replace('/* INSERT_FUNCTIONS */', funcs)
    with tempfile.TemporaryDirectory(prefix='konoha-cpucp-fast-') as directory:
        c = Path(directory) / 'fast.c'
        exe = c.with_suffix('')
        c.write_text(harness)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-sign-compare', '-g', '-O1',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(c), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True)
    return {'status': 'PASS', 'cases': int(result.stdout.strip()),
            'sanitizers': 'ASan+UBSan', 'source_sha256': {
                SOURCE: hashlib.sha256(path.read_bytes()).hexdigest()},
            'limits': ['WALT/mailbox service stubs verify hint control flow; physical scheduler migration needs device verification.']}


if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
