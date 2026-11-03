#!/usr/bin/env python3
"""Run the complete adapted CPUSS v4 driver with bounded MMIO/debugfs stubs."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import sys
HERE = Path(__file__).resolve().parent
SOURCE = 'drivers/soc/qcom/qcom_cpuss_sleep_stats_v4.c'
def verify(root):
    p = Path(root) / SOURCE
    source = re.sub(r'(?m)^#include[^\n]*\n', '', p.read_text())
    harness = (HERE / 'tests/cpuss_v4_harness.c').read_text().replace('/* INSERT_DRIVER */', source)
    with tempfile.TemporaryDirectory(prefix='onyx-cpuss-v4-') as directory:
        c = Path(directory) / 'cpuss.c'
        exe = c.with_suffix('')
        c.write_text(harness)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-unused-variable', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', str(c), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=20)
    return {'status':'PASS', 'cases':int(result.stdout.strip()), 'sanitizers':'ASan+UBSan',
            'source_sha256':{SOURCE:hashlib.sha256(p.read_bytes()).hexdigest()},
            'limits':['MMIO and debugfs/devres are bounded host stubs; native counter timing and actual debugfs proxy lifetime remain kernel/device checks.']}
if __name__ == '__main__':
    print(json.dumps(verify(Path(sys.argv[1])), indent=2))
