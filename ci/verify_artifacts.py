#!/usr/bin/env python3
"""Validate the complete build inventory before exporting or releasing it."""
import hashlib
import json
from pathlib import PurePosixPath
import re


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relative(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or str(path) != name:
        raise ValueError('Unsafe artifact path: ' + name)
    return path


def verify(artifact, source_root):
    report = json.loads((artifact / 'report.json').read_text())
    expected = json.loads((source_root / 'port/modules.json').read_text())
    wanted = [entry['output'] for entry in expected]
    records = report['module_verification']
    names = [entry['module'] for entry in records]
    if len(set(wanted)) != len(wanted) or len(set(names)) != len(names) or set(names) != set(wanted):
        raise ValueError('Incomplete or duplicated platform module inventory')
    if {path.name for path in (artifact / 'platform').glob('*.ko')} != set(wanted):
        raise ValueError('Unexpected platform module files')
    hashes = report['sha256']
    actual = set()
    for path in artifact.rglob('*'):
        if path.is_symlink():
            raise ValueError('Unexpected artifact symlink')
        if path.is_file():
            actual.add(path.relative_to(artifact).as_posix())
    if actual != set(hashes) | {'report.json'} or 'report.json' in hashes:
        raise ValueError('Build hash inventory does not cover every artifact')
    for name, checksum in hashes.items():
        relative(name)
        if not re.fullmatch('[0-9a-f]{64}', checksum) or digest(artifact / name) != checksum:
            raise ValueError('Build artifact changed: ' + name)
    pins = json.loads((artifact / 'inputs/sources.json').read_text())
    committed = json.loads((source_root / 'port/sources.json').read_text())
    if pins != committed:
        raise ValueError('Build source pins differ from the selected source')
    if json.loads((artifact / 'inputs/modules.json').read_text()) != expected:
        raise ValueError('Build module catalog differs from the selected source')
    release = report['kernel_release']
    if not release.startswith(pins['common']['kernel'] + '-android17-'):
        raise ValueError('Built kernel release differs from source pins')
    for entry in records:
        name = 'platform/' + entry['module']
        if hashes.get(name) != entry['sha256'] or not entry['vermagic'].startswith(release + ' '):
            raise ValueError('Module hash or vermagic differs: ' + entry['module'])
        if not isinstance(entry['imports_checked'], int) or entry['imports_checked'] < 0:
            raise ValueError('Module import verification is missing')
    ksu = report['kernelsu_next']
    if not all(re.fullmatch('[0-9a-f]{40}', value) for value in (pins['common']['revision'], ksu['revision'])):
        raise ValueError('Build must record exact GKI and KernelSU revisions')
    archived_ksu = json.loads((artifact / 'inputs/kernelsu-next/source.json').read_text())
    if any(ksu.get(key) != value for key, value in archived_ksu.items()):
        raise ValueError('KernelSU identity differs from the archived source')
    if digest(artifact / 'inputs/kernelsu-next/source.tar') != ksu['sha256']:
        raise ValueError('KernelSU source archive changed')
    config = (artifact / 'gki/kernel_aarch64_dot_config').read_text().splitlines()
    if 'CONFIG_KSU=y' not in config or 'CONFIG_MODVERSIONS=y' not in config:
        raise ValueError('Built kernel lacks KernelSU or module versioning')
    if any('CONFIG_' + option + '=y' in config for option in ('KSU_DEBUG', 'KSU_DISABLE_MANAGER', 'KSU_DISABLE_POLICY')):
        raise ValueError('Unexpected KernelSU configuration')
    if (artifact / 'gki/kernel.release').read_text().strip() != release:
        raise ValueError('Kernel release file differs from build report')
    for name in ('gki/Image', 'gki/kernel_aarch64_Module.symvers', 'devicetrees/dtb.img', 'devicetrees/dtbo.img'):
        if not (artifact / name).stat().st_size:
            raise ValueError('Empty required build artifact: ' + name)
    dt = report['devicetree_verification']
    dt_report = json.loads((artifact / 'devicetrees/report.json').read_text())
    for key in ('overlay_engines_agree', 'bootloader_ids_preserved', 'dtbo_table_verified'):
        if dt.get(key) is not True or dt_report.get(key) is not True:
            raise ValueError('Device-tree verification failed: ' + key)
    trees = json.loads((source_root / 'port/devicetrees.json').read_text())['dtb_order']
    if set(dt_report['trees']) != set(trees):
        raise ValueError('Device-tree variants are incomplete')
    for name in trees:
        if not (artifact / 'devicetrees/checked' / relative(name)).is_file():
            raise ValueError('Checked device tree is missing: ' + name)
    return report, pins, len(wanted)
