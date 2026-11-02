#!/usr/bin/env python3
"""Materialize or verify the complete GKI source, including required patches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(os.environ.get('KONOHA_SOURCE_ROOT', Path(__file__).resolve().parents[1]))


def hashes(root):
    result = {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            result[name] = {'symlink': os.readlink(path)}
        elif path.is_file():
            with path.open('rb') as stream:
                result[name] = {'sha256': hashlib.file_digest(stream, 'sha256').hexdigest(),
                                'executable': bool(path.stat().st_mode & stat.S_IXUSR)}
    return result


def snapshot(workspace, write=False):
    port = ROOT / 'port'
    pins = json.loads((port / 'sources.json').read_text())
    common = workspace / 'common'
    revision = subprocess.check_output(['git', '-C', str(common), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != pins['common']['revision']:
        raise ValueError('GKI snapshot input does not match the pinned upstream revision')
    sys.path.insert(0, str(port))
    from common_fixes import applied
    with tempfile.TemporaryDirectory(prefix='konoha-gki-snapshot-') as temporary:
        root = Path(temporary)
        archive = root / 'source.tar'
        with archive.open('wb') as stream:
            subprocess.run(['git', '-C', str(common), 'archive', '--format=tar', revision],
                           stdout=stream, check=True)
        tree = root / 'common'
        tree.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(tree, filter='data')
        archive.unlink()
        with applied(tree, root / 'patch-receipt'):
            source = hashes(tree)
            destination = ROOT / 'common'
            manifest = port / 'common-source-hashes.json'
            if write:
                if destination.exists():
                    if destination.is_symlink() or (any(destination.iterdir()) and
                            (not manifest.is_file() or hashes(destination) != json.loads(manifest.read_text()))):
                        raise ValueError('Preserve local GKI snapshot edits before regenerating sources')
                    shutil.rmtree(destination)
                shutil.copytree(tree, destination, symlinks=True)
                manifest.write_text(json.dumps(source, indent=2) + '\n')
            else:
                expected = json.loads(manifest.read_text())
                if source != expected or hashes(destination) != expected:
                    raise ValueError('GKI snapshot differs from exact upstream/required-patch replay')
    return {'gki_files_verified': len(source), 'upstream_revision': revision}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('workspace', type=Path)
    p.add_argument('--write', action='store_true')
    a = p.parse_args()
    print(json.dumps(snapshot(a.workspace.resolve(), a.write), indent=2))
