"""Deduplicate stopped compiler caches, restoring independent files before use.

GNU tar records hardlinked files once.  These links exist only while the
disposable runner saves its cache; restoring separate files is essential because
Kbuild may rewrite one target's generated headers without changing another's.
"""
from collections import defaultdict
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


@contextmanager
def writable_parent(path):
    parent = path.parent
    mode = stat.S_IMODE(parent.stat().st_mode)
    changed = not mode & stat.S_IWUSR
    if changed:
        parent.chmod(mode | stat.S_IWUSR)
    try:
        yield
    finally:
        if changed:
            parent.chmod(mode)


def stable(path, previous):
    current = path.lstat()
    if not stat.S_ISREG(current.st_mode) or any(
            getattr(current, field) != getattr(previous, field)
            for field in ('st_dev', 'st_ino', 'st_size', 'st_mode', 'st_uid', 'st_gid', 'st_mtime_ns')):
        raise ValueError('Compiler cache changed after Bazel stopped: ' + str(path))


def deduplicate(parent, roots, manifest):
    """Link byte-identical regular files, recording each original timestamp."""
    by_size = defaultdict(list)
    for root in roots:
        for directory, _, filenames in os.walk(root, followlinks=False):
            for name in filenames:
                path = Path(directory) / name
                info = path.lstat()
                # Preserve existing hardlink relationships; never follow symlinks.
                if stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size >= 4096:
                    key = (info.st_dev, info.st_size, info.st_mode, info.st_uid, info.st_gid)
                    by_size[key].append((path, info))
    groups = []
    for candidates in by_size.values():
        if len(candidates) < 2:
            continue
        by_digest = defaultdict(list)
        for path, info in candidates:
            stable(path, info)
            checksum = digest(path)
            stable(path, info)
            by_digest[checksum].append((path, info))
        for checksum, copies in by_digest.items():
            if len(copies) > 1:
                groups.append((checksum, copies))
    record = {'version': 1, 'roots': [str(root.relative_to(parent)) for root in roots], 'groups': []}
    for checksum, copies in groups:
        record['groups'].append({
            'sha256': checksum, 'bytes': copies[0][1].st_size,
            'mode': stat.S_IMODE(copies[0][1].st_mode),
            'files': [{'path': str(path.relative_to(parent)), 'mtime_ns': info.st_mtime_ns,
                       'atime_ns': info.st_atime_ns} for path, info in copies],
        })
    manifest.write_text(json.dumps(record, separators=(',', ':')) + '\n')
    linked, saved = 0, 0
    for _, copies in groups:
        canonical, original = copies[0]
        for duplicate, info in copies[1:]:
            stable(canonical, original)
            stable(duplicate, info)
            with writable_parent(duplicate):
                handle, name = tempfile.mkstemp(prefix='.onyx-cache-', dir=duplicate.parent)
                os.close(handle)
                temporary = Path(name)
                try:
                    temporary.unlink()
                    os.link(canonical, temporary, follow_symlinks=False)
                    os.replace(temporary, duplicate)
                finally:
                    temporary.unlink(missing_ok=True)
            linked += 1
            saved += info.st_blocks * 512
    return {'deduplicated_files': linked, 'duplicate_bytes_removed': saved,
            'content_groups': len(groups), 'manifest': str(manifest)}


def restore(parent, roots, manifest, reserve_bytes=1024**3):
    """Validate the archive, then undo its temporary hardlinks and timestamps."""
    record = json.loads(manifest.read_text())
    expected_roots = [str(root.relative_to(parent)) for root in roots]
    if record.get('version') != 1 or record.get('roots') != expected_roots or not isinstance(record.get('groups'), list):
        raise ValueError('Unexpected compiler cache link manifest')
    seen, groups, needed = set(), [], 0
    for group in record['groups']:
        if (type(group.get('bytes')) is not int or group['bytes'] < 4096 or
                type(group.get('mode')) is not int or not 0 <= group['mode'] <= 0o7777 or
                not isinstance(group.get('sha256'), str) or len(group['sha256']) != 64 or
                not isinstance(group.get('files'), list) or len(group['files']) < 2):
            raise ValueError('Invalid compiler cache link group')
        paths = []
        for entry in group['files']:
            name = entry.get('path')
            if not isinstance(name, str) or not name or Path(name).is_absolute() or '..' in Path(name).parts or name in seen:
                raise ValueError('Invalid or duplicate compiler cache path')
            seen.add(name)
            path = parent / name
            if path.is_symlink() or path.resolve() != path or not any(path.is_relative_to(root) for root in roots):
                raise ValueError('Compiler cache link leaves its cache roots')
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_size != group['bytes'] or stat.S_IMODE(info.st_mode) != group['mode']:
                raise ValueError('Restored compiler cache file differs: ' + name)
            if any(type(entry.get(key)) is not int or entry[key] < 0 for key in ('mtime_ns', 'atime_ns')):
                raise ValueError('Invalid compiler cache timestamp')
            if paths and not os.path.samefile(paths[0][0], path):
                raise ValueError('Compiler cache hardlink group differs from its manifest')
            paths.append((path, entry))
        if digest(paths[0][0]) != group['sha256']:
            raise ValueError('Compiler cache content checksum differs')
        needed += group['bytes'] * (len(paths) - 1)
        groups.append(paths)
    if shutil.disk_usage(parent).free < needed + reserve_bytes:
        raise ValueError('Insufficient space to restore independent compiler cache files')
    restored = 0
    for paths in groups:
        for path, entry in paths[1:]:
            with writable_parent(path):
                handle, name = tempfile.mkstemp(prefix='.onyx-cache-', dir=path.parent)
                os.close(handle)
                temporary = Path(name)
                try:
                    shutil.copyfile(path, temporary)
                    temporary.chmod(stat.S_IMODE(path.stat().st_mode))
                    os.utime(temporary, ns=(entry['atime_ns'], entry['mtime_ns']))
                    os.replace(temporary, path)
                finally:
                    temporary.unlink(missing_ok=True)
            restored += 1
        canonical, entry = paths[0]
        os.utime(canonical, ns=(entry['atime_ns'], entry['mtime_ns']))
    manifest.unlink()
    return {'restored_independent_files': restored, 'expanded_bytes': needed}
