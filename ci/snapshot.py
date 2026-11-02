#!/usr/bin/env python3
"""Store or verify the complete modified source tree against its replayed recipe."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def hashes(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file() and p.name != '.staged-files.json'}


def snapshot(staged, write=False):
    source = hashes(staged)
    if not source or not (staged / 'BUILD.bazel').exists():
        raise ValueError('Staged source tree is incomplete')
    if write:
        destination = ROOT / 'modules'
        # This script operates only on the repository's generated snapshot.
        shutil.rmtree(destination)
        shutil.copytree(staged, destination, ignore=shutil.ignore_patterns('.staged-files.json'))
        (ROOT / 'ci/module-source-hashes.json').write_text(json.dumps(source, indent=2) + '\n')
    else:
        expected = json.loads((ROOT / 'ci/module-source-hashes.json').read_text())
        if hashes(ROOT / 'modules') != expected or source != expected:
            raise ValueError('Modified module snapshot differs from exact source/patch replay')
    return {'files_verified': len(source), 'modules': len(json.loads((ROOT / 'port/modules.json').read_text()))}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('staged', type=Path)
    p.add_argument('--write', action='store_true')
    a = p.parse_args()
    print(json.dumps(snapshot(a.staged, a.write), indent=2))
