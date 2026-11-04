#!/usr/bin/env python3
"""Fast-forward the source branch to a verified LTS candidate and record it."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
TRACK = json.loads((ROOT / 'ci/upstream.json').read_text())


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], cwd=ROOT, text=True))


def record_result(repository, kernel, kernelsu, source):
    from record_success import record
    return record(repository, kernel, kernelsu, source)


def promote(repository, branch, source, kernel, kernelsu, run_url, record_state=True):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid repository')
    if not re.fullmatch(r'upstream/android17-6\.18-[0-9a-f]{12}-[0-9a-f]{12}', branch):
        raise ValueError('Only tracker-created source candidates can be promoted')
    if not all(re.fullmatch('[0-9a-f]{40}', value) for value in (source, kernel, kernelsu)):
        raise ValueError('Promotion requires exact source, kernel and KernelSU revisions')
    match = re.fullmatch(r'https://github\.com/' + re.escape(repository) + r'/actions/runs/(\d+)', run_url)
    if not match:
        raise ValueError('Build URL must identify this repository')
    prefix = 'repos/' + repository
    jobs = api(prefix + '/actions/runs/' + match[1] + '/jobs')['jobs']
    if not any(job['name'] == 'build' and job['conclusion'] == 'success' for job in jobs):
        raise ValueError('Full build job has not succeeded')
    status = api(prefix + '/commits/' + source + '/status')['statuses']
    checks = [entry for entry in status if entry['context'] == 'Konoha / GKI and modules']
    if not checks or checks[0]['state'] != 'success' or checks[0]['target_url'] != run_url:
        raise ValueError('Source commit lacks this build\'s successful verification status')
    content = api(prefix + '/contents/port/sources.json?ref=' + source)
    pins = json.loads(base64.b64decode(content['content']))
    if pins['common']['revision'] != kernel:
        raise ValueError('Built kernel does not match candidate source pins')
    if not re.fullmatch(r'6\.18\.\d+', pins['common']['kernel']):
        raise ValueError('Candidate is outside Linux 6.18')
    base = api(prefix + '/git/ref/heads/' + TRACK['source_branch'])['object']['sha']
    if base == source:
        return {'status': 'Verified candidate already accepted; release can recover',
                'source_revision': source, 'kernel': pins['common']['kernel'], 'promoted': True}
    owner = repository.split('/')[0]
    proposals = api(prefix + '/pulls?state=open&head=' + quote(owner + ':' + branch, safe='')
                    + '&base=' + quote(TRACK['source_branch'], safe=''))
    if len(proposals) != 1 or proposals[0]['draft']:
        return {'status': 'No single open non-draft update PR; preserving source branch'}
    proposal = proposals[0]
    if proposal['head']['sha'] != source:
        return {'status': 'Candidate changed after build; a new build is required'}
    target = 'refs/heads/' + TRACK['source_branch']
    comparison = api(prefix + '/compare/' + base + '...' + source)
    if comparison['status'] != 'ahead':
        return {'status': 'Source branch changed after candidate replay; a new candidate is required'}
    # Push the tested commit itself. The lease atomically checks the base we
    # just validated, preventing a concurrent update from entering an untested
    # merge tree. An ancestry check above limits this to fast-forward updates.
    subprocess.run(['git', 'fetch', '--depth=64', 'origin', target, source], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    result = subprocess.run(['git', 'push', '--force-with-lease=' + target + ':' + base,
                             'origin', source + ':' + target], cwd=ROOT, text=True, capture_output=True)
    if result.returncode:
        current = api(prefix + '/git/ref/heads/' + TRACK['source_branch'])['object']['sha']
        if current != base:
            return {'status': 'Source advanced during promotion; preserving its changes'}
        raise RuntimeError(result.stderr)
    # Release publication records state only after all release assets exist.
    state = record_result(repository, kernel, kernelsu, source) if record_state else None
    updated = api(prefix + '/pulls/' + str(proposal['number']))
    return {'status': ('Verified source fast-forwarded; update PR merged' if updated.get('merged')
                       else 'Verified source fast-forwarded; update PR closure pending'),
            'source_revision': source, 'kernel': pins['common']['kernel'], 'promoted': True,
            'url': proposal['html_url'], 'build_state': state}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('branch')
    p.add_argument('source')
    p.add_argument('kernel')
    p.add_argument('kernelsu')
    p.add_argument('run_url')
    p.add_argument('--no-record', action='store_true', help='record inputs only after release publication')
    a = p.parse_args()
    result = promote(os.environ['GITHUB_REPOSITORY'], a.branch, a.source,
                     a.kernel, a.kernelsu, a.run_url, record_state=not a.no_record)
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write('promoted=' + str(result.get('promoted', False)).lower() + '\n')
    print(json.dumps(result, indent=2))
