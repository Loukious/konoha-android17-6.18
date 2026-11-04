"""Apply reviewed common fixes, including new files, then restore the pinned tree."""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
INPUTS = HERE / 'common-patches'


def _contents(path):
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError(f'Preserve unexpected common source type: {path}')
    return path.read_bytes() if path.exists() else None


def _sha(contents):
    return hashlib.sha256(contents).hexdigest() if contents is not None else None


@contextmanager
def applied(common, archive):
    manifest = json.loads((INPUTS / 'manifest.json').read_text())
    patch = INPUTS / manifest['patch']
    if _sha(patch.read_bytes()) != manifest['patch_sha256']:
        raise ValueError('Required common patch changed without review')
    before = {}
    for name, hashes in manifest['files'].items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Unsafe common patch path: {name}')
        before[name] = _contents(common / name)
        if _sha(before[name]) != hashes['before_sha256']:
            raise ValueError(f'Preserve changed common source before patching: {name}')
    archive.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(patch, archive / patch.name)
    receipt = dict(manifest, status='Required common fixes applied; build running')
    completed = False
    candidate = None
    try:
        if patch.stat().st_size:
            subprocess.run(['git', '-C', str(common), 'apply', '--check', str(patch)], check=True)
            subprocess.run(['git', '-C', str(common), 'apply', str(patch)], check=True)
        candidate = {name: _contents(common / name) for name in before}
        for name, contents in candidate.items():
            if contents is None or _sha(contents) != manifest['files'][name]['candidate_sha256']:
                raise ValueError(f'Common patch replay mismatch: {name}')
            target = archive / 'source' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        yield
        completed = True
    finally:
        if candidate is not None:
            # Refuse restoration if another writer changed any patched source.
            for name, contents in candidate.items():
                if _contents(common / name) != contents:
                    receipt['status'] = f'Concurrent common edit preserved: {name}'
                    (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
                    raise ValueError(f'Common changed during build; preserve it: {name}')
            for name, contents in before.items():
                path = common / name
                if contents is None:
                    path.unlink()
                else:
                    path.write_bytes(contents)
        receipt['status'] = ('Required fixes built and archived; pinned common files restored exactly'
                             if completed else 'Build failed; pinned common files restored exactly')
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
