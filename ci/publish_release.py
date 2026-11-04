#!/usr/bin/env python3
"""Publish complete verified build assets, then let CI record accepted inputs."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

from verify_artifacts import digest, relative, verify

TRACK = json.loads((Path(__file__).resolve().parent / 'upstream.json').read_text())
FILES = {
    'report.json', 'build.log', 'konoha-gki-and-modules.tar.gz',
    'kernel-abi/Module.symvers', 'kernel-abi/kernel.release', 'kernel-abi/kernel_aarch64_dot_config',
}


def run(args):
    return subprocess.check_output(args, text=True).strip()


def api(path, optional=False):
    result = subprocess.run(['gh', 'api', path], text=True, capture_output=True)
    if optional and result.returncode and '(HTTP 404)' in result.stderr:
        return None
    if result.returncode:
        raise RuntimeError(result.stderr)
    return json.loads(result.stdout)


def listed(path, key=None):
    """Read all pages; authenticated inventories include unpublished drafts."""
    page = 1
    while True:
        response = api(path + ('&' if '?' in path else '?') + f'per_page=100&page={page}')
        entries = response[key] if key else response
        if not isinstance(entries, list):
            raise ValueError('GitHub inventory is not a list')
        yield from entries
        if len(entries) < 100:
            return
        page += 1


def release_by_tag(prefix, tag):
    release = api(prefix + '/releases/tags/' + tag, optional=True)
    if release:
        return release
    matches = [release for release in listed(prefix + '/releases') if release.get('tag_name') == tag]
    if len(matches) > 1:
        raise ValueError('Release tag matches multiple releases')
    return matches[0] if matches else None


def release_inputs(release, tag, source, kernel, kernelsu):
    if (type(release.get('id')) is not int or release['id'] < 1 or release.get('tag_name') != tag or
            release.get('target_commitish') != source or
            not isinstance(release.get('body'), str) or
            any(value not in release.get('body', '') for value in (source, kernel, kernelsu))):
        raise ValueError('Existing release tag identifies different inputs')


def validate_bundle(bundle, source_root, source, kernel, kernelsu, run_id, attempt):
    identity = json.loads((bundle / 'release.json').read_text())
    expected = {'source_revision': source, 'common_revision': kernel, 'kernelsu_revision': kernelsu,
                'run_id': str(run_id), 'run_attempt': str(attempt),
                'artifact_name': f'Konoha-Android17-6.18-{run_id}-{attempt}'}
    if any(identity.get(key) != value for key, value in expected.items()):
        raise ValueError('Release bundle belongs to different build inputs or run')
    actual = {p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file()}
    if actual != FILES | {'release.json', 'SHA256SUMS'} or any(p.is_symlink() for p in bundle.rglob('*')):
        raise ValueError('Release bundle inventory is incomplete or unsafe')
    hashes = {}
    for line in (bundle / 'SHA256SUMS').read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not match or match[2] in hashes:
            raise ValueError('Invalid or duplicated release checksum')
        relative(match[2])
        hashes[match[2]] = match[1]
    if set(hashes) != actual - {'SHA256SUMS'} or set(identity['sha256']) != FILES:
        raise ValueError('Release checksums do not cover every asset')
    for name, checksum in hashes.items():
        if digest(bundle / name) != checksum:
            raise ValueError('Release asset changed: ' + name)
    if any(hashes[name] != checksum for name, checksum in identity['sha256'].items()):
        raise ValueError('Release identity and checksums differ')
    root_name = str(relative(identity['artifact_root']))
    if '/' in root_name:
        raise ValueError('Invalid archive root')
    with tempfile.TemporaryDirectory(prefix='konoha-release-verify-') as temporary:
        root = Path(temporary)
        with tarfile.open(bundle / 'konoha-gki-and-modules.tar.gz', 'r:gz') as archive:
            seen = set()
            for member in archive.getmembers():
                name = str(relative(member.name.rstrip('/')))
                if name in seen or name.split('/')[0] != root_name or not (member.isfile() or member.isdir()):
                    raise ValueError('Unsafe or duplicated build archive member')
                seen.add(name)
            archive.extractall(root, filter='data')
        artifact = root / root_name
        report, pins, count = verify(artifact, source_root)
        if report.get('qdss_diagnostic') is not False:
            raise ValueError('Diagnostic builds cannot become automatic releases')
        if (pins['common']['revision'], report['kernelsu_next']['revision'], report['kernel_release'], count) != (
                kernel, kernelsu, identity['kernel_release'], identity['modules']):
            raise ValueError('Archived build identity differs from release metadata')
        if digest(artifact / 'report.json') != hashes['report.json'] or digest(artifact / 'build.log') != hashes['build.log']:
            raise ValueError('Exported evidence differs from the build archive')
        for exported, archived in (
                ('Module.symvers', 'kernel_aarch64_Module.symvers'),
                ('kernel.release', 'kernel.release'), ('kernel_aarch64_dot_config', 'kernel_aarch64_dot_config')):
            if digest(bundle / 'kernel-abi' / exported) != digest(artifact / 'gki' / archived):
                raise ValueError('Exported ABI differs from the build archive')
    return identity


def publish(repository, bundle, source_root, source, kernel, kernelsu, run_id, attempt):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid repository')
    if not all(re.fullmatch('[0-9a-f]{40}', value) for value in (source, kernel, kernelsu)):
        raise ValueError('Release requires exact source revisions')
    if not str(run_id).isdigit() or not str(attempt).isdigit():
        raise ValueError('Release requires an exact Actions run and attempt')
    prefix = 'repos/' + repository
    build_run = api(prefix + '/actions/runs/' + str(run_id) + '/attempts/' + str(attempt))
    if build_run['head_branch'] != TRACK['ci_branch'] or build_run['path'] != '.github/workflows/build.yml':
        raise ValueError('Release must come from the CI build workflow')
    jobs = api(prefix + '/actions/runs/' + str(run_id) + '/attempts/' + str(attempt) + '/jobs')['jobs']
    if not any(job['name'] == 'build' and job['conclusion'] == 'success' for job in jobs):
        raise ValueError('Full build and artifact upload have not succeeded')
    identity = validate_bundle(bundle, source_root, source, kernel, kernelsu, run_id, attempt)
    accepted = api(prefix + '/git/ref/heads/' + TRACK['source_branch'])['object']['sha']
    if accepted != source:
        raise ValueError('Source branch advanced or candidate was not promoted; refusing release')
    version = identity['kernel_release'].split('-')[0]
    tag = f'gki-{version}-s{source[:12]}-k{kernelsu[:12]}'
    assets = sorted(bundle / name for name in FILES | {'release.json', 'SHA256SUMS'})
    asset_names = {path.name for path in assets}
    existing = release_by_tag(prefix, tag)
    if existing:
        release_inputs(existing, tag, source, kernel, kernelsu)
        if not existing['draft']:
            if {asset['name'] for asset in existing['assets']} != asset_names or any(asset['size'] < 1 for asset in existing['assets']):
                raise ValueError('Published release has incomplete assets; refusing to overwrite it')
            return {'status': 'verified inputs already released', 'tag': tag, 'url': existing['html_url']}
    url = f'https://github.com/{repository}/actions/runs/{run_id}'
    notes = (
        f'Automatically built Android 17 GKI **{identity["kernel_release"]}**, '
        f'KernelSU Next dev, and all **{identity["modules"]} Onyx platform modules.\n\n'
        f'- Source commit: `{source}`\n- Google GKI revision: `{kernel}`\n'
        f'- KernelSU Next revision: `{kernelsu}`\n- Verified build: {url} (attempt {attempt})\n\n'
        'Complete source replay, module import CRCs, artifact hashes and both device-tree variants passed. '
        'The archive includes matching kernel, modules, device trees, ABI, configuration, source inputs and logs; '
        'release.json and SHA256SUMS identify the exact assets.\n\n'
        'These are build artifacts. Device boot and hardware validation are separate; '
        'installation requires matched boot/DLKM images for the ROM.\n'
    )
    with tempfile.TemporaryDirectory(prefix='konoha-release-notes-') as temporary:
        path = Path(temporary) / 'notes.md'
        path.write_text(notes)
        if not existing:
            run(['gh', 'release', 'create', tag, '--repo', repository, '--target', source, '--draft',
                 '--title', f'Android 17 GKI {version} / KernelSU Next', '--notes-file', str(path)])
            existing = release_by_tag(prefix, tag)
            if not existing:
                raise ValueError('Created draft release is missing from the authenticated inventory')
            release_inputs(existing, tag, source, kernel, kernelsu)
        release_id = existing['id']
        draft = api(prefix + '/releases/' + str(release_id))
        release_inputs(draft, tag, source, kernel, kernelsu)
        if draft['id'] != release_id or draft.get('draft') is not True:
            raise ValueError('Selected release is no longer the expected draft')
        run(['gh', 'release', 'upload', tag, *map(str, assets), '--repo', repository, '--clobber'])
        uploaded = api(prefix + '/releases/' + str(release_id))
        release_inputs(uploaded, tag, source, kernel, kernelsu)
        if uploaded['id'] != release_id or uploaded.get('draft') is not True:
            raise ValueError('Uploaded release is no longer the expected draft')
        if {asset['name'] for asset in uploaded['assets']} != asset_names:
            raise ValueError('Draft release upload is incomplete')
        for asset in uploaded['assets']:
            local = next(path for path in assets if path.name == asset['name'])
            if asset['size'] != local.stat().st_size or asset.get('digest') not in (None, 'sha256:' + digest(local)):
                raise ValueError('Uploaded release asset differs: ' + asset['name'])
        # A source push may land during a long upload. Publishing an older
        # draft as Latest must not supersede a newer accepted-source release.
        accepted = api(prefix + '/git/ref/heads/' + TRACK['source_branch'])['object']['sha']
        if accepted != source:
            raise ValueError('Source branch advanced during upload; leaving the release unpublished')
        run(['gh', 'release', 'edit', tag, '--repo', repository, '--draft=false', '--latest', '--notes-file', str(path)])
    return {'status': 'verified build released', 'tag': tag,
            'url': f'https://github.com/{repository}/releases/tag/{tag}'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('source')
    parser.add_argument('kernel')
    parser.add_argument('kernelsu')
    parser.add_argument('--build-run-id', default=os.environ.get('GITHUB_RUN_ID'))
    parser.add_argument('--build-run-attempt', default=os.environ.get('BUILD_RUN_ATTEMPT'))
    args = parser.parse_args()
    result = publish(os.environ['GITHUB_REPOSITORY'], args.bundle,
                     Path(os.environ['KONOHA_SOURCE_ROOT']), args.source, args.kernel, args.kernelsu,
                     args.build_run_id, args.build_run_attempt)
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write('release_tag=' + result['tag'] + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
