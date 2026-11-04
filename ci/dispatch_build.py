#!/usr/bin/env python3
"""Deduplicate exact-input builds and allow bounded recovery after failures."""
import argparse
import json
import re
import subprocess


def should_dispatch(builds):
    if any(build['status'] != 'completed' or build['conclusion'] == 'success' for build in builds):
        return False
    # A broken source/ABI needs repair, not an unlimited daily build loop.
    return len(builds) < 3


def dispatch(source, kernelsu):
    if not all(re.fullmatch('[0-9a-f]{40}', value) for value in (source, kernelsu)):
        raise ValueError('Dispatch requires exact detected source and KernelSU revisions')
    title = f'Build android17-6.18 @ {source} / KSU {kernelsu}'
    builds = json.loads(subprocess.check_output(
        ['gh', 'run', 'list', '--workflow', 'build.yml', '--branch', 'ci', '--limit', '100',
         '--json', 'displayTitle,status,conclusion,url'], text=True))
    matches = [build for build in builds if build['displayTitle'] == title]
    if not should_dispatch(matches):
        return {'status': 'exact-input build active, successful, or retry limit reached', 'builds': matches}
    subprocess.run(['gh', 'workflow', 'run', 'build.yml', '--ref', 'ci',
                    '-f', 'source_ref=android17-6.18', '-f', 'source_revision=' + source,
                    '-f', 'kernelsu_revision=' + kernelsu], check=True)
    return {'status': 'exact-input build dispatched', 'source_revision': source, 'kernelsu_revision': kernelsu}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('kernelsu')
    args = parser.parse_args()
    print(json.dumps(dispatch(args.source, args.kernelsu), indent=2))
