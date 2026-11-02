"""Apply the reviewed common-kernel diagnostic only during the build."""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
EVIDENCE = HERE / 'validation/clock_suspend_resume'


@contextmanager
def applied(common, archive):
    checked = json.loads((EVIDENCE / 'source-validation.json').read_text())
    before, candidate = {}, {}
    for name, hashes in checked['files'].items():
        path = common / name
        before[name] = path.read_bytes()
        candidate[name] = (EVIDENCE / 'candidate' / name).read_bytes()
        if path.is_symlink() or hashlib.sha256(before[name]).hexdigest() != hashes['before_sha256']:
            raise ValueError(f'Preserve changed common source before diagnostic: {name}')
        if hashlib.sha256(candidate[name]).hexdigest() != hashes['candidate_sha256']:
            raise ValueError(f'Diagnostic candidate changed: {name}')
    archive.mkdir(parents=True, exist_ok=False)
    patch = EVIDENCE / 'common-qdss-diagnostic.patch'
    shutil.copyfile(patch, archive / patch.name)
    subprocess.run(['git', '-C', str(common), 'apply', '--check', str(patch)], check=True)
    subprocess.run(['git', '-C', str(common), 'apply', str(patch)], check=True)
    receipt = dict(checked, patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest())
    try:
        for name, contents in candidate.items():
            if (common / name).read_bytes() != contents:
                raise ValueError(f'Diagnostic patch replay mismatch: {name}')
            target = archive / 'source' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
        receipt['status'] = 'Exact diagnostic patch applied; build running'
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        yield
    finally:
        # Never overwrite a concurrent edit, even after a failed build.
        for name, contents in candidate.items():
            if (common / name).read_bytes() != contents:
                raise ValueError(f'Common changed during diagnostic; preserve it: {name}')
        for name, contents in before.items():
            (common / name).write_bytes(contents)
        receipt['status'] = 'Diagnostic build source archived; original common files restored exactly'
        (archive / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
