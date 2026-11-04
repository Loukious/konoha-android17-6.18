#!/usr/bin/env python3
"""Exercise actual boot-stat mapping and epoch hook registration/lifetime paths."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
HERE = Path(__file__).resolve().parent
SOURCES = ('drivers/soc/qcom/boot_stats.c', 'drivers/soc/qcom/msm_show_epoch.c')
def verify(staged):
    sources = {name: (Path(staged)/name).read_text() for name in SOURCES}
    drivers = '\n'.join(re.sub(r'(?m)^#include[^\n]*\n', '', source) for source in sources.values())
    harness = (HERE/'tests/boot_diagnostics_harness.c').read_text().replace('/* INSERT_DRIVERS */', drivers)
    with tempfile.TemporaryDirectory(prefix='konoha-boot-diag-') as directory:
        c = Path(directory)/'diag.c'; exe=c.with_suffix(''); c.write_text(harness)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(c),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],check=True,text=True,capture_output=True)
    return {'status':'PASS','cases':int(result.stdout.strip()),'sanitizers':'ASan+UBSan',
            'source_sha256':{name:hashlib.sha256((Path(staged)/name).read_bytes()).hexdigest() for name in SOURCES},
            'limits':['Host MMIO/hook stubs verify bounds and cleanup; physical boot timestamps and suspend logs need device validation.']}
if __name__ == '__main__': print(json.dumps(verify(sys.argv[1]),indent=2))
