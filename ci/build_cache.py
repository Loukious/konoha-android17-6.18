#!/usr/bin/env python3
"""Make archive space on disposable runners and verify a cache was really saved."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from urllib.parse import urlencode

import cache_files


def context(environment):
    if any(environment.get(key) != value for key, value in (
            ('GITHUB_ACTIONS', 'true'), ('RUNNER_ENVIRONMENT', 'github-hosted'), ('RUNNER_OS', 'Linux'))):
        raise ValueError('Cache cleanup requires a disposable GitHub-hosted Linux runner')
    temporary = Path(environment['RUNNER_TEMP']).resolve(strict=True)
    checkout = Path(environment['GITHUB_WORKSPACE']).resolve(strict=True)
    workspace = temporary / 'konoha-workspace'
    if not workspace.is_dir() or workspace.is_symlink() or workspace.resolve() != workspace:
        raise ValueError('Unexpected kernel workspace')
    return temporary, checkout, workspace


def cache_roots(temporary, workspace):
    bases = sorted(path.parent for path in (workspace / 'out/bazel/output_user_root').glob('*/action_cache'))
    cache = [temporary / 'kleaf-cache']
    for base in bases:
        cache += [base / 'action_cache', base / 'execroot/_main/bazel-out']
    if not bases or any(not path.is_dir() or path.is_symlink() or path.resolve() != path for path in cache):
        raise ValueError('Expected the complete stopped Kleaf/Bazel cache')
    return bases, cache


def prepare(environment):
    temporary, checkout, workspace = context(environment)
    bundle = checkout / 'artifacts'
    identity = json.loads((bundle / 'release.json').read_text())
    if identity.get('run_id') != environment['GITHUB_RUN_ID'] or identity.get('run_attempt') != environment['GITHUB_RUN_ATTEMPT']:
        raise ValueError('Only the current uploaded build can be cleaned for caching')
    bases, cache = cache_roots(temporary, workspace)
    manifest = temporary / 'kleaf-cache-links.json'
    if manifest.is_symlink() or (manifest.exists() and not manifest.is_file()):
        raise ValueError('Invalid compiler cache manifest path')
    # Upload and `bazel shutdown` precede this step in the workflow. These copies
    # are no longer consumed by this job; the release job downloads the immutable
    # uploaded bundle. Retain every Kleaf object and Bazel action/output cache.
    discard = [bundle, workspace / 'artifacts', workspace / '.repo']
    discard += [base / 'sandbox' for base in bases]
    for path in discard:
        parent = checkout if path == bundle else workspace
        if path.is_symlink() or not path.resolve().is_relative_to(parent):
            raise ValueError('Refusing cache cleanup through a symlink or outside its workspace')
        if path.exists() and not path.is_dir():
            raise ValueError('Expected a disposable directory: ' + str(path))
        if any(kept.resolve().is_relative_to(path.resolve()) for kept in cache):
            raise ValueError('Cache cleanup would remove reusable compiler outputs')
    before = shutil.disk_usage(checkout).free
    removed = []
    for path in discard:
        if path.exists():
            shutil.rmtree(path)
            removed.append(str(path))
    deduplication = cache_files.deduplicate(temporary, cache, manifest)
    after = shutil.disk_usage(checkout).free
    return {'free_bytes_before': before, 'free_bytes_after': after,
            'reclaimed_bytes': after - before, 'removed': removed,
            'retained_cache_paths': [str(path) for path in cache], 'deduplication': deduplication}


def restore(environment):
    temporary, _, workspace = context(environment)
    manifest = temporary / 'kleaf-cache-links.json'
    if not manifest.exists():
        if environment.get('CACHE_MATCHED_KEY'):
            raise ValueError('Restored cache is missing its independent-file manifest')
        return {'restored_independent_files': 0, 'cold_build': True}
    if manifest.is_symlink() or not manifest.is_file():
        raise ValueError('Invalid compiler cache manifest path')
    _, cache = cache_roots(temporary, workspace)
    return cache_files.restore(temporary, cache, manifest)


def saved(repository, key, query=None, delay=time.sleep):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) or not re.fullmatch(r'kleaf-v3-[A-Za-z0-9_.-]+', key):
        raise ValueError('Invalid cache repository or key')
    if query is None:
        def query(path):
            return json.loads(subprocess.check_output(['gh', 'api', path], text=True))
    path = 'repos/' + repository + '/actions/caches?' + urlencode({'key': key, 'per_page': 100})
    for attempt in range(6):
        matches = [entry for entry in query(path)['actions_caches']
                   if entry.get('key') == key and entry.get('ref') == 'refs/heads/ci']
        if len(matches) == 1 and type(matches[0].get('size_in_bytes')) is int and matches[0]['size_in_bytes'] > 0:
            return {name: matches[0][name] for name in ('id', 'key', 'ref', 'size_in_bytes', 'created_at')}
        if attempt != 5:
            delay(5)
    raise ValueError('GitHub did not retain the build cache; the next build cannot reuse it')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('prepare')
    sub.add_parser('restore')
    confirm = sub.add_parser('confirm')
    confirm.add_argument('key')
    args = parser.parse_args()
    if args.command == 'prepare':
        result = prepare(os.environ)
    elif args.command == 'restore':
        result = restore(os.environ)
    else:
        try:
            result = saved(os.environ['GITHUB_REPOSITORY'], args.key)
        except ValueError:
            if os.environ.get('GITHUB_STEP_SUMMARY'):
                with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
                    stream.write('Compilation cache was **not retained**. The next build will be cold; inspect the cache-save log.\n')
            raise
        if os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
                stream.write(f'Confirmed saved Kleaf cache `{result["key"]}` ({result["size_in_bytes"] / 2**30:.2f} GiB).\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
