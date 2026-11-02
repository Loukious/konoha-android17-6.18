#!/usr/bin/env python3
"""Rebase common fixes and diagnostics by exact patch replay; reject conflicts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'port'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def replay(repo, patch, reverse=False):
    args = ['git', '-C', str(repo), 'apply', '--check']
    if reverse:
        args.append('--reverse')
    return subprocess.run(args + ['-'], input=patch, capture_output=True).returncode == 0


def refresh(common):
    patch_dir = PORT / 'common-patches'
    original = (patch_dir / 'upstream-platform-remove.patch').read_bytes()
    pieces = re.split(rb'(?=diff --git )', original)
    header, diffs = pieces[0], pieces[1:]
    active, integrated, files = [], [], {}
    with tempfile.TemporaryDirectory(prefix='onyx-patch-rebase-') as temporary:
        root = Path(temporary)
        for diff in diffs:
            name = diff.splitlines()[0].split()[2].decode()[2:]
            source = common / name
            data = source.read_bytes()
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            if replay(root, diff):
                subprocess.run(['git', '-C', str(root), 'apply', '-'], input=diff, check=True)
                active.append(diff)
            elif replay(root, diff, reverse=True):
                integrated.append(name)
            else:
                raise ValueError(f'Common patch conflicts with upstream: {name}')
            files[name] = {'before_sha256': sha(data), 'candidate_sha256': sha(target.read_bytes())}
        patch = header + b''.join(active) if active else b''
        manifest = json.loads((patch_dir / 'manifest.json').read_text())
        manifest.update(files=files, patch_sha256=sha(patch), integrated_upstream=integrated)
        # Refresh opt-in diagnostic sources, also requiring exact application.
        evidence = PORT / 'validation/clock_suspend_resume'
        diagnostic = (evidence / 'common-qdss-diagnostic.patch').read_bytes()
        receipt = json.loads((evidence / 'source-validation.json').read_text())
        for name in receipt['files']:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((common / name).read_bytes())
        if not replay(root, diagnostic):
            raise ValueError('QDSS diagnostic conflicts with upstream; manual port required')
        subprocess.run(['git', '-C', str(root), 'apply', '-'], input=diagnostic, check=True)
        for name in receipt['files']:
            receipt['files'][name] = {'before_sha256': sha((common / name).read_bytes()),
                                    'candidate_sha256': sha((root / name).read_bytes())}
        # Commit refreshed inputs only after every patch has replayed cleanly.
        (patch_dir / manifest['patch']).write_bytes(patch)
        (patch_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for name in receipt['files']:
            (evidence / 'candidate' / name).write_bytes((root / name).read_bytes())
        (evidence / 'source-validation.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return {'active_common_patches': len(active), 'already_integrated_upstream': integrated}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('common', type=Path)
    print(json.dumps(refresh(p.parse_args().common), indent=2))
