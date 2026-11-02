#!/usr/bin/env python3
"""Record the exact successful main-branch build for change detection."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def record(repository, kernel, kernelsu):
    if not all(re.fullmatch('[0-9a-f]{40}', value) for value in (kernel, kernelsu)):
        raise ValueError('Successful build must have exact source revisions')
    # A newer main may land during a long build. Never overwrite its state
    # with the successful result of an older recipe/workflow checkout.
    expected = os.environ.get('GITHUB_SHA')
    if expected:
        current = json.loads(subprocess.check_output(
            ['gh', 'api', f'repos/{repository}/git/ref/heads/main'], text=True))
        if current['object']['sha'] != expected:
            return {'status': 'main advanced; preserving newer build state'}
    url = f'repos/{repository}/contents/ci/last-built.json'
    previous = subprocess.run(['gh', 'api', url], capture_output=True, text=True)
    if previous.returncode and '404' not in previous.stderr:
        raise RuntimeError(previous.stderr)
    metadata = json.loads(previous.stdout) if previous.returncode == 0 else {}
    state = json.dumps({'common_revision': kernel, 'kernelsu_revision': kernelsu}, indent=2) + '\n'
    if metadata and base64.b64decode(metadata['content']).decode() == state:
        return
    payload = {'message': 'Record successful kernel and KernelSU dev build [skip ci]',
               'content': base64.b64encode(state.encode()).decode(), 'branch': 'main'}
    if metadata:
        payload['sha'] = metadata['sha']
    with tempfile.TemporaryDirectory(prefix='konoha-success-') as temporary:
        path = Path(temporary) / 'request.json'
        path.write_text(json.dumps(payload))
        subprocess.run(['gh', 'api', url, '--method', 'PUT', '--input', str(path)],
                       stdout=subprocess.DEVNULL, check=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel')
    p.add_argument('kernelsu')
    a = p.parse_args()
    record(os.environ['GITHUB_REPOSITORY'], a.kernel, a.kernelsu)
