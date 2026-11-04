#!/usr/bin/env python3
"""Select a successful owned build artifact for release with current CI code."""
import argparse
import json
import os
from pathlib import Path
import re

import publish_release as release


def numbers(run_id, attempt):
    if any(not re.fullmatch(r'[1-9][0-9]*', str(value)) for value in (run_id, attempt)):
        raise ValueError('Recovery requires a positive build run ID and attempt')


def select(repository, run_id, attempt):
    numbers(run_id, attempt)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid repository')
    prefix = 'repos/' + repository
    attempt_path = prefix + f'/actions/runs/{run_id}/attempts/{attempt}'
    run = release.api(attempt_path)
    if (type(run.get('id')) is not int or run['id'] != int(run_id) or
            type(run.get('run_attempt')) is not int or run['run_attempt'] != int(attempt) or
            run.get('repository', {}).get('full_name') != repository or
            run.get('head_repository', {}).get('full_name') != repository or
            any(type(run.get(key, {}).get('id')) is not int or run[key]['id'] < 1
                for key in ('repository', 'head_repository')) or
            run.get('head_branch') != release.TRACK['ci_branch'] or
            run.get('path') != '.github/workflows/build.yml' or
            not isinstance(run.get('head_sha'), str) or
            not re.fullmatch(r'[0-9a-f]{40}', run.get('head_sha', ''))):
        raise ValueError('Recovery build is not the exact owned CI workflow attempt')
    builds = [job for job in release.listed(attempt_path + '/jobs', 'jobs') if job.get('name') == 'build']
    if len(builds) != 1 or builds[0].get('conclusion') != 'success' or builds[0].get('status') != 'completed':
        raise ValueError('Recovery requires a completed successful full build job')
    # A failed release job makes the overall run fail; only the full build
    # job and its immutable attempt-specific upload are required here.
    name = f'Konoha-Android17-6.18-{run_id}-{attempt}'
    matches = [artifact for artifact in release.listed(prefix + f'/actions/runs/{run_id}/artifacts', 'artifacts')
               if artifact.get('name') == name]
    if len(matches) != 1:
        raise ValueError('Recovery requires exactly one attempt-specific build artifact')
    artifact = matches[0]
    owner = artifact.get('workflow_run', {})
    if (type(artifact.get('id')) is not int or artifact['id'] < 1 or
            artifact.get('expired') is not False or type(artifact.get('size_in_bytes')) is not int or
            artifact['size_in_bytes'] < 1 or
            owner.get('id') != int(run_id) or owner.get('head_branch') != run['head_branch'] or
            owner.get('head_sha') != run['head_sha'] or
            owner.get('repository_id') != run['repository'].get('id') or
            owner.get('head_repository_id') != run['head_repository'].get('id')):
        raise ValueError('Recovery artifact is expired or belongs to different build inputs')
    return {'build_run_id': str(run_id), 'build_attempt': str(attempt),
            'artifact_id': str(artifact['id']), 'artifact_name': name, 'ci_revision': run['head_sha']}


def inputs(bundle, run_id, attempt):
    numbers(run_id, attempt)
    path = bundle / 'release.json'
    if path.is_symlink() or not path.is_file():
        raise ValueError('Recovery bundle identity is missing or unsafe')
    identity = json.loads(path.read_text())
    expected = {'run_id': str(run_id), 'run_attempt': str(attempt),
                'artifact_name': f'Konoha-Android17-6.18-{run_id}-{attempt}'}
    if any(identity.get(key) != value for key, value in expected.items()):
        raise ValueError('Recovery bundle belongs to a different run or attempt')
    result = {output: identity.get(key, '') for output, key in (
        ('source_revision', 'source_revision'), ('kernel_revision', 'common_revision'),
        ('kernelsu_revision', 'kernelsu_revision'))}
    if any(not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value) for value in result.values()):
        raise ValueError('Recovery bundle requires exact source, GKI and KernelSU revisions')
    # These values select the source checkout; publish() subsequently replays
    # the complete bundle/hash/ABI validation and checks the accepted head.
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    select_parser = sub.add_parser('select')
    select_parser.add_argument('run_id')
    select_parser.add_argument('attempt')
    inputs_parser = sub.add_parser('inputs')
    inputs_parser.add_argument('bundle', type=Path)
    inputs_parser.add_argument('run_id')
    inputs_parser.add_argument('attempt')
    args = parser.parse_args()
    result = (select(os.environ['GITHUB_REPOSITORY'], args.run_id, args.attempt)
              if args.command == 'select' else inputs(args.bundle, args.run_id, args.attempt))
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            for key, value in result.items():
                stream.write(key + '=' + value + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
