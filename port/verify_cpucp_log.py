#!/usr/bin/env python3
"""Exercise real CPUCP/PDP mailbox log parsing and firmware controls."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = 'drivers/soc/qcom/cpucp_log.c'


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
    definitions = source[source.index('#define MAX_PRINT_SIZE'):source.index('#define CREATE_TRACE_POINTS')]
    definitions += source[source.index('#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)'):source.index('static DEFINE_MUTEX(cpucp_dirs_lock)')]
    funcs = '\n'.join(function(source, name) for name in (
        'flush_cpucp_log', 'set_log_level', 'get_log_level', 'get_module_ids',
        'set_module_log_level', 'set_log_enabled', 'get_log_enabled',
        'get_last_newline', 'print_to_trace', 'cpucp_log_work',
        'get_free_buffer', 'cpucp_log_rx', 'populate_free_buffers', 'cpucp_log_stop'))
    harness = (HERE / 'tests/cpucp_log_harness.c').read_text()
    harness = harness.replace('/* INSERT_DECLARATIONS */', definitions)
    harness = harness.replace('/* INSERT_FUNCTIONS */', funcs)
    with tempfile.TemporaryDirectory(prefix='konoha-cpucp-log-') as directory:
        c = Path(directory) / 'log.c'
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
            'limits': ['Stubbed mailbox/SCMI control flow and native payloads; physical firmware delivery needs device validation.']}


if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
