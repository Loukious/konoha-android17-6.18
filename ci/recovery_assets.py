#!/usr/bin/env python3
"""Build a recovery installer that repacks the installed Onyx ROM's own images."""
import fnmatch
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import struct
import tarfile
import tempfile

import installer_assets

HERE = Path(__file__).resolve().parent / 'recovery'
ROOT = HERE.parents[1]
SOFT_OMISSIONS = {'subsys_pil_tz', 'qcom_arm_smmu_mod', 'phy_msm_snps_hs'}


def names(release):
    installer_assets.names(release)
    return [f'Onyx-Complete-{release}.zip', 'full-recovery.json']


def norm(name):
    return PurePosixPath(name).name.removesuffix('.ko').replace('-', '_')


def read(path):
    return json.loads(path.read_text())


def digest(data):
    return hashlib.sha256(data).hexdigest()


def modinfo(path):
    result = {}
    for record in subprocess.check_output(['modinfo', '-0', str(path)]).decode().split('\0'):
        if not record:
            continue
        # kmod versions differ: newer -0 output exposes raw key=value records,
        # while older versions retain the formatted key: value representation.
        match = re.fullmatch(r'([A-Za-z0-9_]+)(?:=|:\s*)(.*)', record, re.DOTALL)
        if not match:
            raise ValueError('Malformed module metadata')
        key, value = match.groups()
        result.setdefault(key.strip(), []).append(value.strip())
    return result


def payload_files(artifact, identity):
    report = read(artifact / 'report.json')
    release = identity['kernel_release']
    if release != report['kernel_release'] or report.get('qdss_diagnostic') is not False:
        raise ValueError('Complete installer requires a verified ordinary kernel release')
    files = {}
    for name, source in {'Image': 'gki/Image', 'dtb.img': 'devicetrees/dtb.img',
                         'dtbo.img': 'devicetrees/dtbo.img', 'modules.builtin': 'gki/modules.builtin',
                         'modules.builtin.modinfo': 'gki/modules.builtin.modinfo'}.items():
        data = (artifact / source).read_bytes()
        if digest(data) != report['sha256'][source]:
            raise ValueError('Release input differs: ' + source)
        files['payload/' + name] = (data, False)
    builtin = {norm(n) for n in files['payload/modules.builtin'][0].decode().splitlines()}
    modules, kinds = {}, {}
    overrides = {norm(row['output']): row['replaces_gki'] for row in read(artifact / 'inputs/modules.json') if 'replaces_gki' in row}
    for path in sorted((artifact / 'platform').glob('*.ko')):
        name = norm(path.name)
        data = path.read_bytes()
        if name in modules or name in builtin or digest(data) != report['sha256']['platform/' + path.name]:
            raise ValueError('Invalid platform module: ' + name)
        modules[name], kinds[name] = (path.name, data), 'platform'
    used = set()
    with tarfile.open(artifact / 'gki/modules_staging_dir.tar.gz') as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unsafe GKI module archive')
            if not member.isfile() or not member.name.endswith('.ko'):
                continue
            name = norm(member.name)
            if name in modules:
                if name not in overrides or name in used or str(path) != f'lib/modules/{release}/' + overrides[name]:
                    raise ValueError('Unreviewed GKI/platform module override: ' + name)
                used.add(name)
                continue
            if name in builtin:
                raise ValueError('Module duplicates a builtin: ' + name)
            modules[name], kinds[name] = (path.name, archive.extractfile(member).read()), 'gki'
    if used != set(overrides):
        raise ValueError('Missing GKI/platform overrides')
    infos = {}
    with tempfile.TemporaryDirectory(prefix='onyx-module-catalog-') as temp:
        for name, (filename, data) in sorted(modules.items()):
            path = Path(temp) / filename
            path.write_bytes(data)
            info = modinfo(path)
            if info.get('vermagic', [''])[0].split()[0] != release or norm(info.get('name', [''])[0]) != name:
                raise ValueError('Module identity differs: ' + filename)
            infos[name] = info
            files['payload/modules/' + filename] = (data, False)
    aliases = {}
    for name, info in infos.items():
        for alias in info.get('alias', []):
            aliases.setdefault(norm(alias), set()).add(name)
    for record in files['payload/modules.builtin.modinfo'][0].decode().split('\0'):
        if '.alias=' in record:
            owner, alias = record.split('.alias=', 1)
            if norm(owner) in builtin:
                aliases.setdefault(norm(alias), set()).add(norm(owner))
    catalog, omitted = {}, {}
    for name, info in sorted(infos.items()):
        depends = [norm(v) for v in info.get('depends', [''])[0].split(',') if v]
        if any(dep not in modules and dep not in builtin for dep in depends):
            raise ValueError('Missing hard dependency: ' + name)
        groups = {'pre': [], 'post': []}
        for row in info.get('softdep', []):
            group = None
            for word in row.split():
                if word in ('pre:', 'post:'):
                    group = word[:-1]
                    continue
                if group is None:  # kmod ignores ungrouped hints.
                    continue
                dependency = norm(word)
                if dependency in modules or dependency in builtin:
                    providers = {dependency}
                else:
                    providers = {owner for pattern, owners in aliases.items()
                                 if fnmatch.fnmatchcase(dependency, pattern) for owner in owners}
                if not providers:
                    if dependency not in SOFT_OMISSIONS:
                        raise ValueError(f'Missing soft dependency of {name}: {word}')
                    omitted.setdefault(name, []).append(dependency)
                groups[group].extend(sorted(providers))
        catalog[name] = {'file': modules[name][0], 'kind': kinds[name], 'depends': depends,
                         'pre': groups['pre'], 'post': groups['post'], 'aliases': info.get('alias', [])}
    definition = read(artifact / 'inputs/nethunter-native-modules.json')
    additions = sorted(norm(n) for n in definition['modules'])
    if any(name not in catalog or kinds[name] != 'gki' for name in additions):
        raise ValueError('Required native feature module is missing')
    firmware_manifest = read(artifact / 'firmware/manifest.json')
    firmware = []
    for item in firmware_manifest['blobs']:
        name = item['path']
        if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
            raise ValueError('Unsafe firmware path')
        data = (artifact / 'firmware/firmware' / name).read_bytes()
        if digest(data) != item['sha256']:
            raise ValueError('Firmware differs: ' + name)
        files['payload/firmware/' + name] = (data, False)
        firmware.append(name)
    if not set(definition.get('wireless_firmware', [])) <= set(firmware):
        raise ValueError('Required native wireless firmware is missing')
    for path in sorted((artifact / 'firmware').rglob('*')):
        if path.is_file() and not path.is_relative_to(artifact / 'firmware/firmware'):
            files['firmware-notices/' + path.relative_to(artifact / 'firmware').as_posix()] = (path.read_bytes(), False)
    parameters = (artifact / 'inputs/boot-parameters.txt').read_text().split()
    config = (artifact / 'gki/kernel_aarch64_dot_config').read_text().splitlines()
    compression = 'zstd,level=9' if 'CONFIG_EROFS_FS_ZIP_ZSTD=y' in config else 'lz4hc,level=12'
    legacy = read(HERE / 'legacy-onyx.json')
    if legacy['format'] != 1 or legacy['device'] != 'onyx' or legacy['legacy_release_prefix'] != '6.6.':
        raise ValueError('Unsupported legacy migration profile')
    for role, lists in legacy['default_loads'].items():
        for filename, entries in lists.items():
            if len(entries) != len(set(entries)) or not set(entries) <= set(catalog) | builtin:
                raise ValueError(f'Legacy migration profile no longer matches this kernel: {role}/{filename}')
    metadata = {'format': 1, 'device': 'onyx', 'kernel_release': release,
                'source_revision': identity['source_revision'], 'common_revision': identity['common_revision'],
                'kernelsu_revision': identity['kernelsu_revision'], 'modules': catalog, 'builtin': sorted(builtin),
                'native_additions': additions, 'boot_parameters': parameters, 'firmware': sorted(firmware),
                'erofs_compression': compression,
                'legacy_onyx': {'module_names': legacy['legacy_module_names'], 'default_loads': legacy['default_loads']},
                'sha256': {name: digest(value[0]) for name, value in sorted(files.items()) if name.startswith('payload/')}}
    files['kernel.json'] = ((json.dumps(metadata, indent=2) + '\n').encode(), False)
    return files, omitted


def tooling_files():
    files = {}
    provenance = read(HERE / 'provenance.json')
    actual = {path.relative_to(HERE).as_posix() for path in HERE.rglob('*')
              if path.is_file() and path != HERE / 'provenance.json' and '__pycache__' not in path.parts}
    if set(provenance['sha256']) != actual:
        raise ValueError('Recovery provenance does not cover the complete source and tool inventory')
    runtime = {'tools/' + name for name in ('onyx-recovery', 'recovery-guard', 'mkfs.erofs', 'fsck.erofs', 'erofs-meta')}
    if not runtime <= set(provenance['sha256']):
        raise ValueError('Recovery tool provenance is incomplete')
    for name, expected in provenance['sha256'].items():
        data = (HERE / name).read_bytes()
        if digest(data) != expected:
            raise ValueError('Recovery tool or source changed without rebuilding: ' + name)
        if name in runtime:
            if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
                raise ValueError('Recovery tool must be ARM64 ELF: ' + name)
            offset = struct.unpack_from('<Q', data, 32)[0]
            stride, count = struct.unpack_from('<HH', data, 54)
            if stride < 56 or offset + stride * count > len(data) or any(
                    struct.unpack_from('<I', data, offset + i * stride)[0] == 3 for i in range(count)):
                raise ValueError('Recovery tool must be static: ' + name)
            files[name] = (data, True)
        else:
            files['installer-source/' + name] = (data, False)
    for name, expected in provenance['build_sha256'].items():
        data = (ROOT / name).read_bytes()
        if digest(data) != expected:
            raise ValueError('Recovery build instructions changed: ' + name)
        files['installer-source/build/' + Path(name).name] = (data, False)
    files['installer-source/provenance.json'] = ((HERE / 'provenance.json').read_bytes(), False)
    anykernel = installer_assets.HERE
    upstream = read(anykernel / 'provenance.json')
    archive_data = (anykernel / 'vendor/AnyKernel3-arm64-tools.tar.gz').read_bytes()
    if (digest(archive_data) != upstream['anykernel_arm64_archive_sha256'] or
            digest((anykernel / 'vendor/AnyKernel3.tar.gz').read_bytes()) != upstream['anykernel_archive_sha256']):
        raise ValueError('AnyKernel tools archive changed')
    with tarfile.open(fileobj=io.BytesIO(archive_data)) as archive:
        for name in ['magiskboot', 'busybox']:
            member = archive.getmember(name)
            if not member.isfile():
                raise ValueError('Invalid recovery tool entry')
            files['tools/' + name] = (archive.extractfile(member).read(), True)
    for name in ['vendor/AnyKernel3.tar.gz', 'vendor/AnyKernel3-arm64-tools.tar.gz', 'provenance.json']:
        files['installer-source/anykernel/' + name] = ((anykernel / name).read_bytes(), False)
    busybox_notice = read(HERE / 'busybox-notices/manifest.json')
    if digest(files['tools/busybox'][0]) != busybox_notice['binary_sha256']:
        raise ValueError('BusyBox source notices do not match the packaged executable')
    return files, provenance


def build(artifact, identity):
    files, omissions = payload_files(artifact, identity)
    tools, provenance = tooling_files()
    files.update(tools)
    files.update({'install.sh': ((HERE / 'install.sh').read_bytes(), True),
                  'META-INF/com/google/android/update-binary': ((HERE / 'update-binary.sh').read_bytes(), True),
                  'META-INF/com/google/android/updater-script': (b'# Onyx complete kernel installer\n', False),
                  'README.md': ((HERE / 'README.md').read_bytes(), False)})
    files['SHA256SUMS'] = (installer_assets.payload(files, files), False)
    archive = installer_assets.zip_bytes(files)
    zip_name, meta_name = names(identity['kernel_release'])
    metadata = {'format': 1, 'source_revision': identity['source_revision'], 'common_revision': identity['common_revision'],
                'kernelsu_revision': identity['kernelsu_revision'], 'kernel_release': identity['kernel_release'],
                'sha256': {zip_name: digest(archive)}, 'modules': len(json.loads(files['kernel.json'][0])['modules']),
                'tools': provenance, 'reviewed_stale_soft_dependencies': omissions,
                'partitions': ['boot', 'vendor_boot', 'recovery', 'dtbo', 'system_dlkm', 'vendor_dlkm'],
                'device_tested': False}
    return {zip_name: archive, meta_name: (json.dumps(metadata, indent=2) + '\n').encode()}


def verify(artifact, bundle, identity):
    for name, data in build(artifact, identity).items():
        if (bundle / name).read_bytes() != data:
            raise ValueError('Complete recovery installer differs from verified inputs: ' + name)


def add_to_bundle(bundle, source):
    import publish_release
    identity = read(bundle / 'release.json')
    publish_release.validate_bundle(bundle, source, identity['source_revision'], identity['common_revision'],
                                    identity['kernelsu_revision'], identity['run_id'], identity['run_attempt'])
    if identity.get('full_recovery'):
        return identity
    with tempfile.TemporaryDirectory(prefix='onyx-complete-installer-') as temp:
        with tarfile.open(bundle / 'konoha-gki-and-modules.tar.gz') as archive:
            archive.extractall(temp, filter='data')  # Complete archive checked above.
        products = build(Path(temp) / identity['artifact_root'], identity)
    for name, data in products.items():
        (bundle / name).write_bytes(data)
    identity['full_recovery'] = sorted(products)
    identity['sha256'].update({name: digest(data) for name, data in products.items()})
    (bundle / 'release.json').write_text(json.dumps(identity, indent=2) + '\n')
    hashes = {p.relative_to(bundle).as_posix(): installer_assets.sha(p.read_bytes())
              for p in sorted(bundle.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS'}
    (bundle / 'SHA256SUMS').write_text(''.join(f'{value}  {name}\n' for name, value in hashes.items()))
    return identity


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    print(json.dumps(add_to_bundle(args.bundle, args.source), indent=2))
