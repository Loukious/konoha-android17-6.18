#!/usr/bin/env python3
"""Compile and exercise the actual bounded native PERF table reader."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = 'drivers/soc/qcom/dcvs/cpufreq_stats_common.h'


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
    declarations = source[source.index('#define PERF_STATS_SIGNATURE'):source.index('static bool stats_range_valid')]
    funcs = '\n'.join(function(source, name) for name in (
        'stats_range_valid', 'stats_domain_geometry', 'stats_read_values',
        'cpufreq_stats_get', 'cpufreq_stats_map'))
    harness = (HERE / 'tests/scmi_stats_harness.c').read_text()
    harness = harness.replace('/* INSERT_DECLARATIONS */', declarations)
    harness = harness.replace('/* INSERT_FUNCTIONS */', funcs)
    with tempfile.TemporaryDirectory(prefix='konoha-scmi-stats-') as directory:
        c = Path(directory) / 'stats.c'
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
            'limits': ['Provider stubs test firmware table parsing; real SCMI metadata and counter updates require hardware.']}


if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
