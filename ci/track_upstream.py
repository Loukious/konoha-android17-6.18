#!/usr/bin/env python3
"""Detect upstream changes and refresh explicit source pins without merging."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PORT = Path(os.environ.get('KONOHA_SOURCE_ROOT', ROOT)) / 'port'
TRACK = json.loads((ROOT / 'ci/upstream.json').read_text())


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()


def remote_head(repository, branch):
    ref = 'refs/heads/' + branch
    result = subprocess.check_output(['git', 'ls-remote', '--exit-code', repository, ref], text=True)
    lines = result.strip().splitlines()
    if len(lines) != 1:
        raise ValueError('Expected exactly one upstream branch')
    revision, actual = lines[0].split()
    if actual != ref or not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Invalid upstream revision')
    return revision


def detect():
    pins = json.loads((PORT / 'sources.json').read_text())
    current = pins['common']['revision']
    latest = remote_head(TRACK['repository'], TRACK['branch'])
    ksu = remote_head(TRACK['kernelsu_repository'], TRACK['kernelsu_branch'])
    source_revision = git(PORT.parent, 'rev-parse', 'HEAD')
    state_path = ROOT / 'ci/last-built.json'
    last = json.loads(state_path.read_text()) if state_path.exists() else {}
    result = {'current': current, 'latest': latest, 'kernelsu': ksu,
              'kernel_changed': latest != current,
              'source_revision': source_revision,
              'source_changed': source_revision != last.get('source_revision'),
              'kernelsu_changed': ksu != last.get('kernelsu_revision') or current != last.get('common_revision')}
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
            for key, value in result.items():
                f.write(f'{key}={str(value).lower() if isinstance(value, bool) else value}\n')
    return result


def update(repo, revision):
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Revision must be a complete commit hash')
    if git(repo, 'rev-parse', 'HEAD') != revision:
        raise ValueError('Updater source does not match detected revision')
    makefile = git(repo, 'show', revision + ':Makefile')
    fields = dict(re.findall(r'^(VERSION|PATCHLEVEL|SUBLEVEL)\s*=\s*(\d+)\s*$', makefile, re.M))
    if (fields.get('VERSION'), fields.get('PATCHLEVEL')) != ('6', '18'):
        raise ValueError('Refusing a kernel outside Linux 6.18')
    pins = json.loads((PORT / 'sources.json').read_text())
    pins['common']['revision'] = revision
    pins['common']['kernel'] = '.'.join(fields[k] for k in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL'))
    imports = json.loads((PORT / 'imports.json').read_text())
    for source in imports:
        if source['project'] == 'common':
            source['revision'] = revision
            source['blob'] = git(repo, 'rev-parse', revision + ':' + source['path'])
    xml = ET.parse(PORT / 'onyx-port.xml')
    entry = xml.getroot().find("extend-project[@name='kernel/common']")
    if entry is None:
        raise ValueError('Common project missing from source manifest')
    entry.set('revision', revision)
    entry.set('upstream', TRACK['branch'])
    # Validate every imported path before changing any recipe file.
    (PORT / 'sources.json').write_text(json.dumps(pins, indent=2) + '\n')
    (PORT / 'imports.json').write_text(json.dumps(imports, indent=2) + '\n')
    ET.indent(xml)
    xml.write(PORT / 'onyx-port.xml', encoding='utf-8', xml_declaration=True)
    return {'revision': revision, 'kernel': pins['common']['kernel']}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--update', type=Path, help='clean common checkout to use for a detected update')
    p.add_argument('--revision')
    a = p.parse_args()
    if bool(a.update) != bool(a.revision):
        p.error('--update and --revision must be supplied together')
    print(json.dumps(update(a.update, a.revision) if a.update else detect(), indent=2))
