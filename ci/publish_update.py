#!/usr/bin/env python3
"""Publish a replayed LTS candidate without overwriting the accepted kernel."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.check_output(args, cwd=ROOT, text=True, **kwargs).strip()


def publish(revision):
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Invalid upstream revision')
    pins = json.loads((ROOT / 'port/sources.json').read_text())
    if pins['common']['revision'] != revision:
        raise ValueError('Candidate does not match detected upstream')
    # Recording build state must not create another candidate for the same
    # recipe. Recipe changes, however, need a fresh branch and complete build.
    base = run(['git', 'rev-parse', 'HEAD:port'])[:12]
    branch = f'upstream/android17-6.18-{revision[:12]}-{base}'
    existing = run(['git', 'ls-remote', 'origin', 'refs/heads/' + branch])
    version = pins['common']['kernel']
    if not existing:
        run(['git', 'checkout', '-b', branch])
        run(['git', 'add', 'port', 'modules', 'ci/module-source-hashes.json'])
        run(['git', 'commit', '-m', f'Track Android 17 LTS {version} ({revision[:12]})'])
        run(['git', 'push', '-u', 'origin', branch])
    # Recover after a failed PR creation or dispatch without overwriting a
    # candidate branch that may contain manual conflict repairs.
    proposals = json.loads(run(['gh', 'pr', 'list', '--head', branch, '--state', 'all',
                               '--json', 'url,state']))
    if proposals and proposals[0]['state'] != 'OPEN':
        return {'status': 'candidate already reviewed; preserving its disposition', 'branch': branch}
    count = len(list((ROOT / 'port/patches').glob('*.patch')))
    modules = len(json.loads((ROOT / 'port/modules.json').read_text()))
    body = (
        f'Update the Onyx port to Android 17 LTS {version} at `{revision}`. '
        'Common-header imports and the complete modified module snapshot are refreshed; '
        f'all {count} module patches must replay exactly. CoreSight changes already in upstream '
        'are detected and excluded from the effective patch.\n\n'
        'The linked build workflow fetches official KernelSU Next dev and builds GKI, '
        f'all {modules} platform modules and both device-tree variants, including symbol/CRC '
        'verification. Conflicts and build failures require a source repair. '
        'No device installation or hardware validation is performed by CI.\n'
    )
    if proposals:
        url = proposals[0]['url']
    else:
        with tempfile.TemporaryDirectory(prefix='konoha-pr-') as temporary:
            path = Path(temporary) / 'body.md'
            path.write_text(body)
            url = run(['gh', 'pr', 'create', '--base', 'main', '--head', branch,
                       '--title', f'Android 17 LTS {version}', '--body-file', str(path)])
    builds = json.loads(run(['gh', 'run', 'list', '--workflow', 'build.yml', '--branch', branch,
                            '--limit', '1', '--json', 'status,conclusion,url']))
    if builds:
        return {'status': 'candidate and build already exist', 'branch': branch,
                'url': url, 'build': builds[0]}
    # GITHUB_TOKEN pushes do not trigger another workflow; dispatch explicitly.
    run(['gh', 'workflow', 'run', 'build.yml', '--ref', branch])
    return {'status': 'candidate PR opened; complete build dispatched', 'branch': branch, 'url': url}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('revision')
    print(json.dumps(publish(p.parse_args().revision), indent=2))
