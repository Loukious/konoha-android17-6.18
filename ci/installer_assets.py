#!/usr/bin/env python3
"""Package immutable verified build outputs as recovery and manager installers."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import re
import stat
import tarfile
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent / 'installers'
WLAN = 'qca_cld3_wcn7750.ko'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def names(release):
    if not re.fullmatch(r'6\.18\.\d+-android17-\d+-[A-Za-z0-9.-]+', release):
        raise ValueError('Unsupported installer kernel release')
    return (f'Konoha-AnyKernel3-{release}.zip', f'nethunter-wlan-injection-{release}.zip')


def abi_table(artifact):
    result = {}
    for path in [artifact / 'gki/kernel_aarch64_Module.symvers', *sorted((artifact / 'platform').glob('*Module.symvers'))]:
        for line in path.read_text().splitlines():
            fields = line.split()
            if len(fields) < 3:
                raise ValueError('Invalid symvers row')
            crc, name = int(fields[0], 16), fields[1]
            if not re.fullmatch(r'[A-Za-z0-9_.$]+', name) or crc > 0xffffffff:
                raise ValueError('Invalid ABI export')
            if name in result and result[name] != crc:
                raise ValueError('Conflicting ABI exports: ' + name)
            result[name] = crc
    if not result:
        raise ValueError('Empty ABI export table')
    return ''.join(f'{crc:08x} {name}\n' for name, crc in sorted(result.items())).encode()


def zip_bytes(files):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, (data, executable) in sorted(files.items()):
            info = zipfile.ZipInfo(name, (2026, 11, 6, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)
    return out.getvalue()


def payload(files, members):
    return ''.join(f'{sha(files[n][0])}  {n}\n' for n in sorted(members)).encode()


def build(artifact, identity):
    report = json.loads((artifact / 'report.json').read_text())
    release = identity['kernel_release']
    ak_name, wlan_name = names(release)
    if report['kernel_release'] != release:
        raise ValueError('Installer release differs from build')
    image = (artifact / 'gki/Image').read_bytes()
    if len(image) < 64 or image[56:60] != b'ARM\x64':
        raise ValueError('Expected uncompressed ARM64 Image')
    records = [r for r in report['module_verification'] if r['module'] == WLAN]
    if len(records) != 1 or records[0]['imports_checked'] <= 0:
        raise ValueError('Verified internal WLAN module is missing')
    wlan = (artifact / 'platform' / WLAN).read_bytes()
    if sha(wlan) != records[0]['sha256'] or b'hdd_monitor_mode_tx_inject' not in wlan:
        raise ValueError('WLAN module does not contain the verified injection implementation')
    deps = records[0]['depends'].split(',')
    if not deps or any(not re.fullmatch('[A-Za-z0-9_-]+', dep) for dep in deps):
        raise ValueError('Invalid WLAN dependencies')
    abi = abi_table(artifact)
    provenance = json.loads((HERE / 'provenance.json').read_text())
    source = (HERE / 'module-abi.c').read_bytes()
    guard = (HERE / 'tools/module-abi').read_bytes()
    upstream = (HERE / 'vendor/AnyKernel3.tar.gz').read_bytes()
    arm64_tools = (HERE / 'vendor/AnyKernel3-arm64-tools.tar.gz').read_bytes()
    if (sha(source) != provenance['module_abi_source_sha256'] or
        sha(guard) != provenance['module_abi_binary_sha256'] or
        sha(upstream) != provenance['anykernel_archive_sha256'] or
        sha(arm64_tools) != provenance['anykernel_arm64_archive_sha256']):
        raise ValueError('Installer tools changed without provenance update')
    substitutions = {'@RELEASE@': release, '@KMI@': release.split('-')[2],
                     '@DEPS@': ' '.join(dep.replace('-', '_') for dep in deps)}
    def template(name):
        value = (HERE / name).read_text()
        for key, replacement in substitutions.items():
            value = value.replace(key, replacement)
        return value.encode()
    metadata = {'format': 1, 'source_revision': identity['source_revision'],
        'common_revision': identity['common_revision'], 'kernelsu_revision': identity['kernelsu_revision'],
        'kernel_release': release, 'kernel_sha256': sha(image), 'wlan_sha256': sha(wlan),
        'abi_sha256': sha(abi), 'anykernel_revision': provenance['anykernel_revision'],
        'anykernel_arm64_revision': provenance['anykernel_arm64_revision'],
        'module_abi_sha256': sha(guard), 'requires': 'Onyx with matching 6.18 modules; kernel-only ZIP is not a 6.6 migration'}
    meta = (json.dumps(metadata, indent=2)+'\n').encode()
    version_code = int(datetime.strptime(identity['artifact_root'][:15], '%Y%m%dT%H%M%S').replace(tzinfo=timezone.utc).timestamp())
    ak = {}
    with tarfile.open(fileobj=io.BytesIO(upstream), mode='r:gz') as archive:
        for member in archive:
            if member.name == 'LICENSE' or member.name == 'README.md' or member.name.startswith(('tools/', 'META-INF/')):
                if member.isdir():
                    continue
                if not member.isfile() or '..' in Path(member.name).parts or member.name.startswith('/'):
                    raise ValueError('Unsafe AnyKernel archive entry')
                ak[member.name] = (archive.extractfile(member).read(), bool(member.mode & 0o111))
    # Onyx is ARM64-only. Upstream master carries 32-bit ARM executables;
    # replace them with the separately pinned official arm64-tools branch.
    with tarfile.open(fileobj=io.BytesIO(arm64_tools), mode='r:gz') as archive:
        for member in archive:
            if not member.isfile() or '/' in member.name or 'tools/' + member.name not in ak:
                raise ValueError('Unexpected AnyKernel ARM64 tool')
            data = archive.extractfile(member).read()
            if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
                raise ValueError('Expected ARM64 AnyKernel executable')
            ak['tools/' + member.name] = (data, True)
    for name, (data, _) in ak.items():
        if name.startswith('tools/') and data.startswith(b'\x7fELF') and data[18:20] != b'\xb7\x00':
            raise ValueError('Non-ARM64 AnyKernel executable: ' + name)
    ak.update({'Image': (image, False), 'anykernel.sh': (template('anykernel.sh'), True),
        'tools/module-abi': (guard, True), 'kernel-abi.tsv': (abi, False),
        'installer.json': (meta, False), 'installer-source/module-abi.c': (source, False),
        'installer-source/provenance.json': ((HERE/'provenance.json').read_bytes(), False)})
    ak['payload.sha256'] = (payload(ak, ['Image', 'kernel-abi.tsv', 'tools/module-abi']), False)
    module = {'wlan/'+WLAN: (wlan, False), 'service.sh': (template('wlan-service.sh'), True),
        'customize.sh': (template('wlan-customize.sh'), False), 'skip_mount': (b'', False),
        'module-abi': (guard, True), 'kernel-abi.tsv': (abi, False), 'installer.json': (meta, False),
        'installer-source/module-abi.c': (source, False),
        'installer-source/provenance.json': ((HERE/'provenance.json').read_bytes(), False),
        'module.prop': ((f'id=nethunter-wlan-injection\nname=NetHunter WLAN Injection (Onyx)\n'
            f'version={release}-s{identity["source_revision"][:12]}\nversionCode={version_code}\n'
            'author=Loukious\ndescription=Matched WCN7750 monitor mode and frame injection. Reboot to activate.\n').encode(), False),
        'META-INF/com/google/android/update-binary': ((HERE/'wlan-update-binary.sh').read_bytes(), True),
        'META-INF/com/google/android/updater-script': (b'#MAGISK\n', False)}
    module['payload.sha256'] = (payload(module, ['wlan/'+WLAN, 'module-abi', 'kernel-abi.tsv']), False)
    products = {ak_name: zip_bytes(ak), wlan_name: zip_bytes(module)}
    metadata['sha256'] = {name: sha(data) for name, data in products.items()}
    products['installers.json'] = (json.dumps(metadata, indent=2)+'\n').encode()
    return products


def verify(artifact, bundle, identity):
    # Recreate expected archives from verified build outputs and pinned tools.
    for name, data in build(artifact, identity).items():
        if (bundle / name).read_bytes() != data:
            raise ValueError('Installer differs from verified inputs: ' + name)


def add_to_bundle(bundle, source):
    import publish_release
    identity = json.loads((bundle / 'release.json').read_text())
    publish_release.validate_bundle(bundle, source, identity['source_revision'], identity['common_revision'],
        identity['kernelsu_revision'], identity['run_id'], identity['run_attempt'])
    if identity.get('installers'):
        return identity
    with tempfile.TemporaryDirectory(prefix='konoha-installer-') as tmp:
        with tarfile.open(bundle / 'konoha-gki-and-modules.tar.gz') as archive:
            archive.extractall(tmp, filter='data')  # Full inventory validated above.
        products = build(Path(tmp) / identity['artifact_root'], identity)
        for name, data in products.items():
            (bundle / name).write_bytes(data)
    identity['installers'] = sorted(products)
    identity['sha256'].update({name: sha(data) for name, data in products.items()})
    (bundle / 'release.json').write_text(json.dumps(identity, indent=2)+'\n')
    hashes = {p.relative_to(bundle).as_posix(): sha(p.read_bytes()) for p in sorted(bundle.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS'}
    (bundle / 'SHA256SUMS').write_text(''.join(f'{v}  {k}\n' for k,v in hashes.items()))
    return identity


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    print(json.dumps(add_to_bundle(args.bundle, args.source), indent=2))
