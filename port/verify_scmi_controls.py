#!/usr/bin/env python3
"""Exercise native C1DCVS/DYNPF controls and complete probe/owner rollback."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCES = ['drivers/soc/qcom/dcvs/c1dcvs_scmi_v2.c', 'drivers/soc/qcom/dcvs/dynpf_scmi.c']


def function(source, name):
    match = re.search(r'(?m)^(?:static )?[^\n;{}]*\b' + name + r'\([^;{}]*\)\n\{', source)
    if not match:
        raise ValueError('Missing function: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def verify(staged):
    hashes, cases = {}, 0
    for relative in SOURCES:
        path = Path(staged) / relative
        source = path.read_text()
        c1 = 'c1dcvs' in relative
        declarations = source[source.index('static struct kobject'):source.index('static int set_enable_c1dcvs' if c1 else 'static ssize_t store_enable_dynpf')]
        if c1:
            declarations += source[source.index('#define to_c1dcvs_attr'):source.index('#define C1DCVS_ATTR_RW')]
        else:
            declarations += re.search(r'(?m)^#define DYNPF_ALGO_STR.*$', source).group() + '\n'
        names = (['set_enable_c1dcvs', 'update_enable_c1dcvs', 'store_enable_c1dcvs',
                  'c1dcvs_enable', 'c1dcvs_parse_threshold', 'set_ipc_thresh'] if c1 else
                 ['store_enable_dynpf', 'show_enable_dynpf'])
        prefix = 'c1dcvs' if c1 else 'dynpf'
        names += ['attr_show', 'attr_store', prefix+'_kobject_release',
                  prefix+'_kobject_cleanup', prefix+'_protocol_cleanup',
                  'scmi_c1dcvs_probe' if c1 else 'qcom_dynpf_probe']
        funcs = '\n'.join(function(source, name) for name in names)
        if c1:
            funcs += '\n' + source[source.index('#define store_c1dcvs_thresh'):source.index('#define show_c1dcvs_thresh')]
            funcs += '\nstore_c1dcvs_thresh(ipc_thresh)\n'
        harness = (HERE / 'tests/scmi_controls_harness.c').read_text()
        harness = ('#define C1_KIND 1\n' if c1 else '#define C1_KIND 0\n') + harness
        harness = harness.replace('/* INSERT_DECLARATIONS */', declarations)
        harness = harness.replace('/* INSERT_FUNCTIONS */', funcs)
        with tempfile.TemporaryDirectory(prefix='konoha-scmi-controls-') as directory:
            c = Path(directory) / 'controls.c'
            exe = c.with_suffix('')
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-sign-compare', '-g', '-O1',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            str(c), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], text=True, capture_output=True, check=True)
            cases += int(result.stdout.strip())
        hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {'status': 'PASS', 'cases': cases, 'sanitizers': 'ASan+UBSan',
            'source_sha256': hashes,
            'limits': ['Real firmware gating/control requests need device verification; host stubs test native commands and lifetime.']}


if __name__ == '__main__':
    print(json.dumps(verify(sys.argv[1]), indent=2))
