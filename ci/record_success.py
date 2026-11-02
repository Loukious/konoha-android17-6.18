#!/usr/bin/env python3
"""Record the exact successful accepted-source build for change detection."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


TRACK = json.loads((Path(__file__).resolve().parent / 'upstream.json').read_text())


def record(repository, kernel, kernelsu, source=None):
    if not all(re.fullmatch('[0-9a-f]{40}', value) for value in (kernel, kernelsu)):
        raise ValueError('Successful build must have exact source revisions')
    # Newer automation may land during a long build. Never overwrite its state
    # with the successful result of an older recipe/workflow checkout.
    expected = os.environ.get('GITHUB_SHA')
    if expected:
        current = json.loads(subprocess.check_output(
            ['gh', 'api', f"repos/{repository}/git/ref/heads/{TRACK['ci_branch']}"], text=True))
        if current['object']['sha'] != expected:
            return {'status': 'CI branch advanced; preserving newer build state'}
    if source is not None:
        if not re.fullmatch('[0-9a-f]{40}', source):
            raise ValueError('Source revision must be a complete Git hash')
        accepted = json.loads(subprocess.check_output(
            ['gh', 'api', f"repos/{repository}/git/ref/heads/{TRACK['source_branch']}"], text=True))
        if accepted['object']['sha'] != source:
            return {'status': 'source branch advanced; preserving accepted build state'}
    url = f'repos/{repository}/contents/ci/last-built.json'
    previous = subprocess.run(['gh', 'api', url + '?ref=' + TRACK['ci_branch']], capture_output=True, text=True)
    if previous.returncode and '404' not in previous.stderr:
        raise RuntimeError(previous.stderr)
    metadata = json.loads(previous.stdout) if previous.returncode == 0 else {}
    state = json.dumps({'common_revision': kernel, 'kernelsu_revision': kernelsu,
                        'source_revision': source}, indent=2) + '\n'
    if metadata and base64.b64decode(metadata['content']).decode() == state:
        return
    payload = {'message': 'Record successful kernel and KernelSU dev build [skip ci]',
               'content': base64.b64encode(state.encode()).decode(), 'branch': TRACK['ci_branch']}
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
    p.add_argument('source')
    a = p.parse_args()
    record(os.environ['GITHUB_REPOSITORY'], a.kernel, a.kernelsu, a.source)
