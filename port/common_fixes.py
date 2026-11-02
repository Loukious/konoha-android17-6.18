"""Apply required common-source fixes for every build, preserving the pinned tree."""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
INPUTS = HERE / 'common-patches'


@contextmanager
def applied(common, archive):
    manifest = json.loads((INPUTS / 'manifest.json').read_text())
    patch = INPUTS / manifest['patch']
    sha = lambda data: hashlib.sha256(data).hexdigest()
    if sha(patch.read_bytes()) != manifest['patch_sha256']:
        raise ValueError('Required common patch changed without review')
    before = {}
    for name, hashes in manifest['files'].items():
        path = common / name
        before[name] = path.read_bytes()
        if path.is_symlink() or sha(before[name]) != hashes['before_sha256']:
            raise ValueError(f'Preserve changed common source before patching: {name}')
    archive.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(patch, archive / patch.name)
    if patch.stat().st_size:
        subprocess.run(['git', '-C', str(common), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(common), 'apply', str(patch)], check=True)
    candidate = {name: (common / name).read_bytes() for name in before}
    receipt = dict(manifest, status='Required common fixes applied; build running')
    completed = False
    try:
        for name, contents in candidate.items():
            if sha(contents) != manifest['files'][name]['candidate_sha256']:
                raise ValueError(f'Common patch replay mismatch: {name}')
            target = archive / 'source' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        yield
        completed = True
    finally:
        # Leave concurrent edits intact rather than replacing unknown source.
        for name, contents in candidate.items():
            if (common / name).read_bytes() != contents:
                raise ValueError(f'Common changed during build; preserve it: {name}')
        for name, contents in before.items():
            (common / name).write_bytes(contents)
        receipt['status'] = ('Required fixes built and archived; pinned common files restored exactly'
                             if completed else 'Build failed; pinned common files restored exactly')
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
