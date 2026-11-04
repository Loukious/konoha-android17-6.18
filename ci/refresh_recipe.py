#!/usr/bin/env python3
"""Rebase common fixes and diagnostics by exact patch replay; reject conflicts."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = Path(os.environ.get('KONOHA_SOURCE_ROOT', ROOT)) / 'port'


def sha(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def safe_path(root, name):
    if not isinstance(name, str):
        raise ValueError('Common patch path must be a string')
    relative = PurePosixPath(name)
    if not name or relative.is_absolute() or '..' in relative.parts or '.git' in relative.parts or str(relative) != name:
        raise ValueError('Unsafe common patch path: ' + name)
    path = Path(root)
    for parent in (path, *path.parents):
        if parent.is_symlink() or (parent.exists() and not parent.is_dir()):
            raise ValueError('Unexpected common source parent type: ' + str(parent))
    for component in relative.parts:
        if path.is_symlink() or (path.exists() and not path.is_dir()):
            raise ValueError('Unexpected common source parent type: ' + str(path))
        path /= component
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError('Unexpected common source type: ' + str(path))
    return path


def contents(root, name):
    path = safe_path(root, name)
    return path.read_bytes() if path.exists() else None


def sections(patch):
    """Keep exact git/unified diff bytes, recognizing headers outside hunks."""
    lines = patch.splitlines(keepends=True)
    starts, old_left, new_left, has_header = [], 0, 0, False
    for index, line in enumerate(lines):
        if old_left or new_left:
            if line.startswith(b'\\'):
                continue
            if line[:1] not in (b' ', b'+', b'-'):
                raise ValueError('Malformed common patch hunk')
            old_left -= line[:1] in (b' ', b'-')
            new_left -= line[:1] in (b' ', b'+')
            if min(old_left, new_left) < 0:
                raise ValueError('Malformed common patch hunk counts')
            continue
        if line.startswith(b'diff --git '):
            starts.append(index)
            has_header = False
        elif line.startswith(b'--- ') and index + 1 < len(lines) and lines[index + 1].startswith(b'+++ '):
            if has_header or not starts:
                starts.append(index)
            has_header = True
        elif line.startswith(b'@@ '):
            match = re.match(rb'@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@', line)
            if not match or not has_header:
                raise ValueError('Malformed common patch hunk header')
            old_left, new_left = (int(value) if value is not None else 1 for value in match.groups())
    if old_left or new_left:
        raise ValueError('Truncated common patch hunk')
    if not starts:
        raise ValueError('Common replay patch has no file diffs')
    header = b''.join(lines[:starts[0]])
    diffs = [b''.join(lines[start:end]) for start, end in zip(starts, starts[1:] + [len(lines)])]
    return header, diffs


def file_headers(diff):
    match = re.search(rb'^--- ([^\n]+)\n\+\+\+ ([^\n]+)\n', diff, re.M)
    if not match:
        raise ValueError('Unsupported common patch file header')
    return tuple(value.split(b'\t', 1)[0].decode() for value in match.groups())


def filename(diff):
    old, new = file_headers(diff)
    if not new.startswith('b/') or (old != '/dev/null' and old != 'a/' + new[2:]):
        raise ValueError('Common fixes require same-path regular files or additions')
    if diff.startswith(b'diff --git '):
        git_header = re.match(rb'diff --git a/(\S+) b/(\S+)\n', diff)
        if not git_header or any(value.decode() != new[2:] for value in git_header.groups()):
            raise ValueError('Git and unified common patch paths differ')
    for mode in re.findall(rb'^(?:(?:new file mode|old mode|new mode|deleted file mode) |index \S+\.\.\S+ )(\d+)$', diff, re.M):
        if mode not in (b'100644', b'100755'):
            raise ValueError('Common patch may not create a nonregular file')
    return new[2:]


def replay(repo, patch, reverse=False):
    args = ['git', '-C', str(repo), 'apply', '--check']
    if reverse:
        args.append('--reverse')
    return subprocess.run(args + ['-'], input=patch, capture_output=True).returncode == 0


def refresh(common):
    patch_dir = PORT / 'common-patches'
    manifest = json.loads(contents(patch_dir, 'manifest.json'))
    original = contents(patch_dir, manifest.get('replay_patch', 'upstream-platform-remove.patch'))
    header, diffs = sections(original)
    names = [filename(diff) for diff in diffs]
    if len(set(names)) != len(names):
        raise ValueError('Duplicate common patch file')
    if 'files' in manifest:
        if not isinstance(manifest['files'], dict):
            raise ValueError('Common patch file inventory must be an object')
        for name in manifest['files']:
            safe_path(common, name)
        if set(names) != set(manifest['files']):
            raise ValueError('Common patch and file inventory differ')
    active, integrated, files = [], [], {}
    with tempfile.TemporaryDirectory(prefix='onyx-patch-rebase-') as temporary:
        root = Path(temporary)
        for name, diff in zip(names, diffs):
            source = safe_path(common, name)
            data = contents(common, name)
            if data is None and file_headers(diff)[0] != '/dev/null':
                raise ValueError('Common patch modifies a missing upstream file: ' + name)
            target = safe_path(root, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            if data is not None:
                target.write_bytes(data)
                target.chmod(source.stat().st_mode & 0o777)
            if replay(root, diff):
                subprocess.run(['git', '-C', str(root), 'apply', '-'], input=diff, check=True)
                active.append(diff)
            elif replay(root, diff, reverse=True):
                integrated.append(name)
            else:
                raise ValueError(f'Common patch conflicts with upstream: {name}')
            files[name] = {'before_sha256': sha(data), 'candidate_sha256': sha(contents(root, name))}
        patch = header + b''.join(active) if active else b''
        manifest.update(files=files, patch_sha256=sha(patch), integrated_upstream=integrated)
        # Refresh opt-in diagnostic sources, also requiring exact application.
        evidence = PORT / 'validation/clock_suspend_resume'
        diagnostic = contents(evidence, 'common-qdss-diagnostic.patch')
        receipt = json.loads(contents(evidence, 'source-validation.json'))
        diagnostic_names = [filename(diff) for diff in sections(diagnostic)[1]]
        if len(set(diagnostic_names)) != len(diagnostic_names) or set(diagnostic_names) != set(receipt['files']):
            raise ValueError('Diagnostic patch and file inventory differ')
        diagnostic_before = {}
        for name in receipt['files']:
            target = safe_path(root, name)
            diagnostic_before[name] = contents(common, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            if diagnostic_before[name] is not None:
                target.write_bytes(diagnostic_before[name])
            elif target.exists():
                raise ValueError('Diagnostic patch overlaps a new common file: ' + name)
        if not replay(root, diagnostic):
            raise ValueError('QDSS diagnostic conflicts with upstream; manual port required')
        subprocess.run(['git', '-C', str(root), 'apply', '-'], input=diagnostic, check=True)
        for name in receipt['files']:
            receipt['files'][name] = {'before_sha256': sha(diagnostic_before[name]),
                                    'candidate_sha256': sha(contents(root, name))}
        # Validate every output before changing any recipe file.
        outputs = [(safe_path(patch_dir, manifest['patch']), patch),
                   (safe_path(patch_dir, 'manifest.json'), (json.dumps(manifest, indent=2) + '\n').encode())]
        for name in receipt['files']:
            target = safe_path(evidence / 'candidate', name)
            data = contents(root, name)
            if data is None:
                raise ValueError('Diagnostic candidate file is missing: ' + name)
            outputs.append((target, data))
        outputs.append((safe_path(evidence, 'source-validation.json'),
                        (json.dumps(receipt, indent=2) + '\n').encode()))
        if len({target for target, _ in outputs}) != len(outputs):
            raise ValueError('Recipe output paths overlap')
        # Commit refreshed inputs only after every patch has replayed cleanly.
        for target, data in outputs:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    return {'active_common_patches': len(active), 'already_integrated_upstream': integrated}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('common', type=Path)
    print(json.dumps(refresh(p.parse_args().common), indent=2))
