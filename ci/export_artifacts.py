#!/usr/bin/env python3
"""Export a verified build, complete modules and matching ABI for Actions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('workspace', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
builds = sorted(d for d in (a.workspace / 'artifacts').iterdir() if (d / 'report.json').is_file())
artifact = builds[-1]
report = json.loads((artifact / 'report.json').read_text())
expected = json.loads((Path(__file__).resolve().parents[1] / 'port/modules.json').read_text())
if len(report['module_verification']) != len(expected):
    raise ValueError('Incomplete module build')
for name, digest in report['sha256'].items():
    if hashlib.sha256((artifact / name).read_bytes()).hexdigest() != digest:
        raise ValueError(f'Build artifact changed: {name}')
a.output.mkdir(parents=True, exist_ok=False)
for name in ['report.json', 'build.log']:
    shutil.copyfile(artifact / name, a.output / name)
with tarfile.open(a.output / 'konoha-gki-and-modules.tar.gz', 'w:gz') as tar:
    tar.add(artifact, arcname=artifact.name)
abi = a.output / 'kernel-abi'
abi.mkdir()
for name in ['kernel_aarch64_Module.symvers', 'kernel.release', 'kernel_aarch64_dot_config']:
    source = artifact / 'gki' / name
    if not source.is_file():
        raise ValueError(f'Missing ABI artifact: {name}')
    shutil.copyfile(source, abi / ('Module.symvers' if name.endswith('_Module.symvers') else name))
manifest = {p.relative_to(a.output).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(a.output.rglob('*')) if p.is_file()}
(a.output / 'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in manifest.items()))
if os.environ.get('GITHUB_OUTPUT'):
    pins = json.loads((artifact / 'inputs/sources.json').read_text())
    with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
        f.write('kernel_revision=' + pins['common']['revision'] + '\n')
        f.write('kernelsu_revision=' + report['kernelsu_next']['revision'] + '\n')
print(json.dumps({'kernel_release': report['kernel_release'], 'modules': len(expected),
                  'kernelsu_dev': report['kernelsu_next']['revision']}, indent=2))
