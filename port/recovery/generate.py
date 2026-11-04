#!/usr/bin/env python3
"""Build five matched Onyx images and a guarded per-ROM recovery ZIP offline."""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import tarfile
import zipfile

from verify_modules import verify

HERE = Path(__file__).resolve().parent
PARTITIONS = ('system_dlkm', 'vendor_dlkm', 'dtbo', 'vendor_boot', 'boot')
EPOCH = 1790812800
# These exact contexts and init/vendor_modprobe permissions were checked against
# the development ROM's binary policy and exercised on that ROM. New policies
# need their own permission audit before enabling this optional bootstrap route.
SERIAL_POLICY_SHA256 = '4bca7122293736ea3470e1f56004d491262e497a439722b6fad192219add6750'
SERIAL_MANIFEST_SHA256 = 'a7031c61417f58f8fd9c22b8c34ba553d7733474958442b20d0e81270a19b96a'
SERIAL_PUBLIC_MANIFEST_SHA256 = '0acd72aadf7e92229fccaa9d5d92bac3fa298901f54cf4902def8af228c016c4'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def static_aarch64(path):
    data = Path(path).read_bytes()
    require(len(data) >= 64 and data[:6] == b'\x7fELF\x02\x01' and
            struct.unpack_from('<H', data, 18)[0] == 183,
            f'AArch64 ELF executable required: {Path(path).name}')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    entsize, count = struct.unpack_from('<HH', data, 54)
    require(entsize == 56 and count > 0 and phoff + count * entsize <= len(data),
            'Invalid ELF program headers')
    require(all(struct.unpack_from('<I', data, phoff + i * entsize)[0] not in (2, 3)
                for i in range(count)), 'Recovery executables must be static with no interpreter')


def sanitized(value, replacements):
    if isinstance(value, str):
        for original, public in sorted(replacements.items(), key=lambda item: len(item[0]), reverse=True):
            value = value.replace(original, public)
        return value
    if isinstance(value, list):
        return [sanitized(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: sanitized(item, replacements) for key, item in value.items()}
    return value


def norm(name):
    return Path(name).name.removesuffix('.ko').replace('-', '_')


def softdep_words(records):
    dependencies, ignored = [], []
    for line in records.splitlines():
        group = None
        skipped = []
        for token in line.split():
            if token in ('pre:', 'post:'):
                group = token
            elif group is not None:
                dependencies.append(norm(token))
            else:
                skipped.append(token)
        if skipped:
            ignored.append(' '.join(skipped))
    return dependencies, ignored


def safe_name(name):
    p = PurePosixPath(name)
    require(not p.is_absolute() and '..' not in p.parts and '\\' not in name,
            f'Unsafe archive path: {name}')
    return p


def extract_release(archive, destination, digest):
    require(re.fullmatch('[0-9a-f]{64}', digest) and sha(archive) == digest,
            'Release archive SHA-256 mismatch')
    destination.mkdir()
    seen = set()
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar:
            p = safe_name(member.name)
            require(str(p) not in seen, f'Duplicate archive member: {member.name}')
            seen.add(str(p))
            require(member.isdir() or member.isfile(), 'Release links/special files are unsupported')
            target = destination / p
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open('xb') as output:
                    shutil.copyfileobj(tar.extractfile(member), output)
    roots = list(destination.iterdir())
    require(len(roots) == 1 and roots[0].is_dir(), 'Expected one artifact root')
    artifact = roots[0]
    report = read(artifact / 'report.json')
    hashes = report['sha256']
    for name, digest in hashes.items():
        safe_name(name)
        require(sha(artifact / name) == digest, f'Artifact hash mismatch: {name}')
    required = ['gki/Image', 'gki/kernel.release', 'gki/modules_staging_dir.tar.gz',
                'gki/kernel_aarch64_Module.symvers', 'gki/modules.builtin',
                'gki/modules.builtin.modinfo', 'devicetrees/dtb.img', 'devicetrees/dtbo.img']
    require(all(name in hashes for name in required), 'Release lacks hashed kernel/module/DT inputs')
    with (artifact / 'gki/Image').open('rb') as stream:
        header = stream.read(64)
    require(len(header) == 64 and header[56:60] == b'ARM\x64', 'Release kernel lacks the ARM64 Image header')
    return artifact


def cpio_entries(data):
    entries, pos = {}, 0
    while pos + 110 <= len(data):
        start = pos
        require(data[pos:pos + 6] == b'070701', 'Only newc vendor ramdisks are supported')
        fields = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        require(namesize > 0 and pos + 110 + namesize <= len(data), 'Invalid CPIO name bounds')
        encoded = data[pos + 110:pos + 110 + namesize]
        require(encoded[-1:] == b'\0' and b'\0' not in encoded[:-1], 'Invalid CPIO name')
        name = encoded[:-1].decode().removeprefix('./')
        safe_name(name)
        pos = (pos + 110 + namesize + 3) & ~3
        require(pos + size <= len(data), 'Invalid CPIO payload bounds')
        payload = data[pos:pos + size]
        pos = (pos + size + 3) & ~3
        require(pos <= len(data), 'Invalid CPIO padding')
        if name == 'TRAILER!!!':
            require(size == 0 and not any(data[pos:]), 'Unexpected trailing CPIO data')
            return entries
        require(name not in entries, f'Duplicate CPIO entry: {name}')
        entries[name] = (data[start:pos], payload, fields)
    raise ValueError('Missing CPIO trailer')


def cpio_record(name, payload, mode, inode):
    encoded = name.encode() + b'\0'
    fields = [inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, EPOCH,
              len(payload), 0, 0, 0, 0, len(encoded), 0]
    raw = b'070701' + ''.join(f'{x:08x}' for x in fields).encode() + encoded
    raw += b'\0' * (-len(raw) % 4)
    raw += payload
    return raw + b'\0' * (-len(raw) % 4)


def nonmodules(name):
    return name != 'lib/modules' and not name.startswith('lib/modules/')


def rebuild_cpio(before, modules):
    retained = {n: entry for n, entry in before.items() if nonmodules(n)}
    require(not any(n.endswith('.ko') for n in retained), 'Unexpected module outside lib/modules')
    inode = max(e[2][0] for e in before.values()) + 1
    content = b''.join(e[0] for e in retained.values())
    content += cpio_record('lib/modules', b'', stat.S_IFDIR | 0o755, inode)
    for path in sorted(modules.iterdir()):
        require(path.is_file() and not path.is_symlink(), 'Module tree must be flat regular files')
        inode += 1
        content += cpio_record('lib/modules/' + path.name, path.read_bytes(),
                               stat.S_IFREG | stat.S_IMODE(path.stat().st_mode), inode)
    content += cpio_record('TRAILER!!!', b'', 0, 0)
    content += b'\0' * (-len(content) % 512)
    after = cpio_entries(content)
    require({n: e[0] for n, e in after.items() if nonmodules(n)} ==
            {n: e[0] for n, e in retained.items()}, 'Non-module CPIO records changed')
    return content


class Builder:
    def __init__(self, output, tools):
        self.output, self.tools = output, tools
        self.commands = []
        (output / 'logs').mkdir()

    def run(self, command):
        command = list(map(str, command))
        self.commands.append(command)
        env = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ.get('PATH', ''))
        r = subprocess.run(command, capture_output=True, env=env)
        (self.output / 'logs' / f'{len(self.commands):03d}-{Path(command[0]).name}.log').write_bytes(r.stdout + r.stderr)
        require(r.returncode == 0, f'{command[0]} failed: {r.stderr.decode(errors="replace")[-2000:]}')
        return r.stdout

    def tool(self, name):
        path = self.tools / name
        require(path.is_file() and os.access(path, os.X_OK), f'Missing host tool: {path}')
        return path

    def unpack(self, image, name):
        directory = self.output / ('unpacked-' + name)
        args = self.run([self.tool('unpack_bootimg'), '--boot_img', image, '--out', directory,
                         '--format', 'mkbootimg', '-0']).decode().split('\0')
        if args[-1] == '':
            args.pop()
        require(args[args.index('--header_version') + 1] == '4', 'Only boot header v4 is supported')
        return args, directory

    def check_ownership(self, image, tree, name):
        owners = {}
        for path in ['/'] + ['/' + str(p.relative_to(tree)) for p in sorted(tree.rglob('*'))]:
            info = self.run([self.tool('dump.erofs'), '--path=' + path, image]).decode()
            match = re.search(r'^Uid:\s*(\d+)\s+Gid:\s*(\d+)\s', info, re.M)
            require(match is not None, f'Cannot read original EROFS owner: {name}:{path}')
            uid, gid = map(int, match.groups())
            require(uid == gid == 0, f'Only root-owned EROFS inputs are supported: {name}:{path} uid={uid} gid={gid}')
            owners[path] = {'uid': uid, 'gid': gid}
        receipt = self.output / (name + '-original-ownership.json')
        receipt.write_text(json.dumps({'image_sha256': sha(image), 'owners': owners}, indent=2) + '\n')
        return {'path': str(receipt), 'sha256': sha(receipt), 'inodes': len(owners)}

    def footer(self, source, target, partition):
        info = self.run([self.tool('avbtool'), 'info_image', '--image', source]).decode()
        def field(name):
            m = re.search(r'^' + re.escape(name) + r':\s*(.+)$', info, re.M)
            require(m is not None, f'Missing AVB field: {name}')
            return m.group(1).strip()
        require(field('Algorithm') in ['NONE', 'SHA256_RSA2048'], 'Unsupported AVB algorithm')
        salt = re.search(r'^\s+Salt:\s*(\S+)', info, re.M)
        require(salt is not None, 'Missing AVB salt')
        hashtree = partition.endswith('dlkm')
        command = [self.tool('avbtool'), 'add_hashtree_footer' if hashtree else 'add_hash_footer',
                   '--partition_name', partition, '--partition_size', str(source.stat().st_size),
                   '--hash_algorithm', 'sha256', '--algorithm', 'NONE', '--salt', salt.group(1),
                   '--rollback_index', field('Rollback Index')]
        if hashtree:
            fec = re.search(r'FEC num roots:\s*(\d+)', info)
            command += ['--fec_num_roots', fec.group(1)] if fec else ['--do_not_generate_fec']
        for key, value in re.findall(r"^\s+Prop: (\S+) -> '(.*)'$", info, re.M):
            command += ['--prop', key + ':' + value]
        maximum = int(self.run(command + ['--calc_max_image_size']).strip())
        require(target.stat().st_size <= maximum,
                f'{partition} cannot fit: {target.stat().st_size} payload bytes, {maximum} maximum. Resize with an independently reviewed layout; no ZIP has been produced.')
        self.run(command + ['--image', target])
        self.run([self.tool('avbtool'), 'verify_image', '--image', target])
        return {'input_algorithm': field('Algorithm'), 'output_algorithm': 'NONE',
                'rollback_index': field('Rollback Index'), 'partition_size': source.stat().st_size,
                'requires_vbmeta_flags': 3}


def option(args, name, value):
    require(args.count(name) == 1, f'Expected one {name}')
    args[args.index(name) + 1] = str(value)


def tree_map(root):
    result = {}
    for path in sorted(root.rglob('*')):
        n = str(path.relative_to(root))
        mode = stat.S_IMODE(path.lstat().st_mode)
        if path.is_symlink():
            result[n] = ('link', os.readlink(path), mode)
        elif path.is_file():
            result[n] = ('file', sha(path), mode)
        elif path.is_dir():
            result[n] = ('dir', mode)
        else:
            raise ValueError(f'Unsupported filesystem entry: {n}')
    return result


def artifact_catalog(artifact, directory):
    directory.mkdir()
    builtin = {norm(n) for n in (artifact / 'gki/modules.builtin').read_text().splitlines()}
    recipe = read(artifact / 'inputs/modules.json')
    overrides = {norm(v['output']): v['replaces_gki'] for v in recipe if 'replaces_gki' in v}
    paths, kinds = {}, {}
    for source in sorted((artifact / 'platform').glob('*.ko')):
        name = norm(source.name)
        require(name not in paths and name not in builtin, 'Duplicate platform module/builtin')
        target = directory / source.name
        shutil.copyfile(source, target)
        paths[name], kinds[name] = target, 'platform'
    used = set()
    release = (artifact / 'gki/kernel.release').read_text().strip()
    with tarfile.open(artifact / 'gki/modules_staging_dir.tar.gz', 'r:gz') as tar:
        for member in tar:
            safe_name(member.name)
            if not member.isfile() or not member.name.endswith('.ko'):
                continue
            name = norm(member.name)
            if name in paths:
                expected = 'lib/modules/' + release + '/' + overrides.get(name, '')
                require(name in overrides and str(PurePosixPath(member.name)) == expected and name not in used,
                        f'Unreviewed platform/GKI override: {name}')
                used.add(name)
                continue
            require(name not in builtin, f'Module duplicates builtin: {name}')
            target = directory / Path(member.name).name
            target.write_bytes(tar.extractfile(member).read())
            paths[name], kinds[name] = target, 'gki'
    require(used == set(overrides), 'Missing reviewed GKI override')
    return paths, kinds, builtin


def assemble_modules(builder, artifact, trees, profile):
    paths, kinds, builtin = artifact_catalog(artifact, builder.output / 'catalog')
    release = (artifact / 'gki/kernel.release').read_text().strip()
    require(re.fullmatch(r'6\.18\.[A-Za-z0-9._+-]+', release), 'Unsupported kernel release')
    verifications = verify(list(paths.values()), [artifact / 'gki/kernel_aarch64_Module.symvers'] +
                           sorted((artifact / 'platform').glob('*_Module.symvers')), release)
    deps, soft, ignored_soft, aliases = {}, {}, {}, {}
    for record in (artifact / 'gki/modules.builtin.modinfo').read_bytes().split(b'\0'):
        if b'.alias=' in record:
            owner, alias = record.decode().split('.alias=', 1)
            if norm(owner) in builtin:
                aliases.setdefault(norm(alias), set()).add(norm(owner))
    for name, path in paths.items():
        deps[name] = [norm(x) for x in builder.run(['modinfo', '-F', 'depends', path]).decode().strip().split(',') if x]
        require(all(n in paths or n in builtin for n in deps[name]), f'Missing hard dependency: {name}')
        soft[name], ignored = softdep_words(builder.run(['modinfo', '-F', 'softdep', path]).decode())
        if ignored:
            ignored_soft[name] = ignored
    # Canonical dependency names take precedence. Only unresolved valid softdep
    # requests need alias lookup; malformed bare hints are recorded like kmod.
    unresolved = {dep for words in soft.values() for dep in words if dep not in paths and dep not in builtin}
    if unresolved:
        for name, path in paths.items():
            for alias in builder.run(['modinfo', '-F', 'alias', path]).decode().splitlines():
                aliases.setdefault(norm(alias), set()).add(name)
    omissions = profile.get('omissions', {})
    require(all(re.fullmatch('[A-Za-z0-9_]+', n) and isinstance(v, str) and len(v) >= 12
                for n, v in omissions.items()), 'Omissions need normalized names and evidence-backed reasons')
    alias_resolutions = {}
    for name, words in soft.items():
        resolved = []
        for dependency in words:
            if dependency in paths or dependency in builtin:
                resolved.append(dependency)
                continue
            providers = {provider for pattern, owners in aliases.items()
                         if fnmatch.fnmatchcase(dependency, pattern) for provider in owners}
            require(providers or dependency in omissions,
                    f'Missing soft dependency of {name}: {dependency}')
            if providers:
                alias_resolutions.setdefault(name, {})[dependency] = sorted(providers)
                resolved.extend(sorted(providers))
        soft[name] = resolved
    builder.soft_dependency_metadata = {'ignored_ungrouped_records': ignored_soft,
                                        'alias_resolutions': alias_resolutions}
    def closure(roots):
        selected, visiting = set(), set()
        def add(name):
            if name in builtin or name in selected:
                return
            require(name in paths, f'Missing module: {name}')
            require(name not in visiting, f'Hard dependency cycle: {name}')
            visiting.add(name)
            for dep in deps[name]:
                add(dep)
            visiting.remove(name)
            selected.add(name)
            for dep in soft[name]:
                if dep in paths and dep not in visiting:
                    add(dep)
        for n in roots:
            add(n)
        return selected
    reports = {}
    layout = profile.get('module_layout')
    layout_root = None
    if layout:
        layout_root = Path(layout['source']).resolve()
        require(sha(layout_root / 'SHA256SUMS') == layout['sha256'], 'Module layout checksum manifest changed')
        for line in (layout_root / 'SHA256SUMS').read_text().splitlines():
            digest, filename = line.split(None, 1)
            safe_name(filename)
            require(sha(layout_root / filename) == digest, f'Module layout hash changed: {filename}')
        meta = read(layout_root / 'prebuilt.json')
        require(meta['device'] == 'onyx' and meta['kernel_release'].startswith('6.18.'), 'Unsupported module layout')
        all_names = {norm(p.name) for p in layout_root.glob('*/lib/modules/*.ko')}
        require(all_names <= set(paths), 'Module layout requests modules absent from this release')
    native_input = artifact / 'inputs/nethunter-native-modules.json'
    native_additions = set()
    if native_input.exists():
        definition = read(native_input)
        require('inputs/nethunter-native-modules.json' in read(artifact / 'report.json')['sha256'],
                'Requested native module definition is not hashed by the artifact')
        native_additions = {norm(name) for name in definition['modules']}
        existing_required = {norm(name) for name in definition['required_existing_modules']}
        require(native_additions | existing_required <= set(paths), 'Required native NetHunter/CAN module is absent')
        require(all(kinds[name] == 'gki' for name in native_additions), 'Native module source kind changed')
    platform_additions = ({n for n, k in kinds.items() if k == 'platform'} - all_names
                          if layout_root else set())
    for partition in ('vendor_ramdisk', 'vendor_dlkm', 'system_dlkm'):
        original = trees[partition] / 'lib/modules'
        require(original.is_dir(), f'Missing baseline modules: {partition}')
        require(all(not p.is_dir() and not p.is_symlink() for p in original.iterdir()),
                f'Only flat module layouts are supported: {partition}')
        requested = {}
        for filename in ['modules.load'] + (['modules.load.recovery'] if partition == 'vendor_ramdisk' else []):
            p = original / filename
            require(p.is_file(), f'Missing baseline load list: {partition}/{filename}')
            names = [norm(n.split('#', 1)[0].strip()) for n in p.read_text().splitlines() if n.split('#', 1)[0].strip()]
            require(all(n in paths or n in builtin or n in omissions for n in names),
                    f'Unresolved ROM load entries: {partition}: {sorted(set(names)-set(paths)-builtin-set(omissions))}')
            requested[filename] = [n for n in names if n in paths]
        native = {norm(p.name) for p in original.glob('*.ko')}
        require(not native - set(paths) - builtin - set(omissions),
                f'Unreviewed missing ROM modules: {sorted(native-set(paths)-builtin-set(omissions))}')
        roots = ([norm(p.name) for p in (layout_root / partition / 'lib/modules').glob('*.ko')]
                 if layout_root else [n for n, kind in kinds.items()
                                     if kind == ('gki' if partition == 'system_dlkm' else 'platform')])
        # Keep the small, previously selected system partition stable. New native
        # feature modules and all new platform modules use the vendor partition;
        # its independently checked capacity remains a hard gate.
        if partition in ['vendor_ramdisk', 'vendor_dlkm']:
            roots += sorted(platform_additions)
        if partition == 'vendor_dlkm':
            roots += sorted(native_additions)
            requested['modules.load'] += sorted((platform_additions | native_additions) - set(requested['modules.load']))
        roots += [n for values in requested.values() for n in values]
        selected = closure(roots)
        root = builder.output / ('depmod-' + partition)
        target = root / 'lib/modules' / release
        target.mkdir(parents=True)
        for name in sorted(selected):
            shutil.copyfile(paths[name], target / paths[name].name)
        for filename in ['modules.builtin', 'modules.builtin.modinfo']:
            shutil.copyfile(artifact / 'gki' / filename, target / filename)
        (target / 'modules.order').write_text(''.join(paths[n].name + '\n' for n in sorted(selected)))
        builder.run(['depmod', '-C', '/dev/null', '-b', root, release])
        for filename, names in requested.items():
            # modprobe resolves the complete hard dependency graph from generated metadata.
            (target / filename).write_text(''.join(paths[n].name + '\n' for n in names))
            for name in names:
                builder.run(['modprobe', '-C', '/dev/null', '-d', root, '-S', release, '--show-depends', name])
        for p in original.glob('modules.*'):
            if p.name.endswith('.blocklist') or p.name in ['modules.blocklist', 'modules.options']:
                shutil.copyfile(p, target / p.name)
        trees[partition] = target
        reports[partition] = {'modules': len(selected), 'load_lists': requested,
                              'new_platform_modules': sorted(platform_additions) if partition != 'system_dlkm' else [],
                              'new_native_modules': sorted(native_additions) if partition == 'vendor_dlkm' else [],
                              'sha256': {p.name: sha(p) for p in target.iterdir() if p.is_file()}}
    return reports, verifications


def make_zip(stage, output):
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    (stage / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + str(p.relative_to(stage)) + '\n' for p in files))
    files.append(stage / 'SHA256SUMS')
    with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            info = zipfile.ZipInfo(str(path.relative_to(stage)), (2026, 11, 4, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | stat.S_IMODE(path.stat().st_mode)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with path.open('rb') as src, archive.open(info, 'w') as dst:
                shutil.copyfileobj(src, dst)
    with zipfile.ZipFile(output) as archive:
        require(archive.testzip() is None, 'ZIP CRC failure')
        for path in files:
            with archive.open(str(path.relative_to(stage))) as stream:
                require(hashlib.file_digest(stream, 'sha256').hexdigest() == sha(path), 'ZIP readback hash mismatch')
    output.with_suffix('.zip.sha256').write_text(sha(output) + '  ' + output.name + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release', type=Path, required=True)
    p.add_argument('--release-sha256', required=True)
    p.add_argument('--profile', type=Path, required=True)
    p.add_argument('--tools', type=Path, required=True)
    p.add_argument('--file-contexts', type=Path, required=True)
    p.add_argument('--busybox', type=Path, required=True)
    p.add_argument('--busybox-notices', type=Path, default=HERE / 'busybox-notices',
                   help='SHA-bound BusyBox notices and corresponding-source distribution')
    p.add_argument('--firmware-package', type=Path,
                   help='Verified firmware/ + manifest.json + licenses/ + sources/ tree')
    p.add_argument('--firmware-manifest-sha256')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    source_names = ['generate.py', 'profile.py', 'verify_modules.py', 'install.sh', 'update-binary',
                    'recovery-guard.c', 'build_guard.sh', 'tools/recovery-guard', 'tools/recovery-guard-build.json',
                    'COPYING', 'GPL-2.0.txt']
    source_hashes = {name: sha(HERE / name) for name in source_names}
    profile = read(a.profile)
    require(profile['device'] == 'onyx', 'Only Onyx has a supported source/device-tree recipe')
    require(not a.output.exists(), 'Use a new output directory')
    images = {n: Path(v['path']).resolve() for n, v in profile['images'].items()}
    require(set(PARTITIONS) | {'init_boot', 'vbmeta'} <= set(images), 'Seven baseline images are required')
    for name, path in images.items():
        entry = profile['images'][name]
        require(path.is_file() and path.stat().st_size == entry['size'] and sha(path) == entry['sha256'],
                f'Baseline image identity changed: {name}')
    vbmeta = images['vbmeta'].read_bytes()
    require(vbmeta[:4] == b'AVB0' and len(vbmeta) >= 124 and struct.unpack_from('>I', vbmeta, 120)[0] & 3 == 3,
            'Baseline must already disable both AVB verification and verity (flags 3)')
    static_aarch64(a.busybox)
    static_aarch64(HERE / 'tools/recovery-guard')
    guard_build = read(HERE / 'tools/recovery-guard-build.json')
    require(guard_build['source_sha256'] == source_hashes['recovery-guard.c'] and
            guard_build['binary_sha256'] == source_hashes['tools/recovery-guard'],
            'Static recovery guard build receipt does not match reviewed source/binary')
    busybox_notice = read(a.busybox_notices / 'manifest.json')
    require(busybox_notice['binary_sha256'] == sha(a.busybox), 'BusyBox source/notices do not match binary')
    for name, digest in busybox_notice['files'].items():
        safe_name(name)
        require(sha(a.busybox_notices / name) == digest, 'BusyBox source/notice identity changed')
    require({str(path.relative_to(a.busybox_notices)) for path in a.busybox_notices.rglob('*')
             if path.is_file()} == set(busybox_notice['files']) | {'manifest.json'},
            'Unmanifested BusyBox notice/source file')
    require(a.file_contexts.is_file(), 'ROM filesystem contexts are required')
    a.output.mkdir(parents=True)
    artifact = extract_release(a.release, a.output / 'release', a.release_sha256)
    builder = Builder(a.output, a.tools.resolve())
    args, boot = builder.unpack(images['boot'], 'boot')
    vendor_args, vendor = builder.unpack(images['vendor_boot'], 'vendor')
    require(vendor_args.count('--vendor_ramdisk_fragment') == 1, 'Only one vendor ramdisk fragment is supported')
    raw = builder.run([builder.tool('lz4'), '-d', '-c', vendor / 'vendor_ramdisk00'])
    before = cpio_entries(raw)
    trees = {'vendor_ramdisk': a.output / 'original-ramdisk'}
    module_root = trees['vendor_ramdisk'] / 'lib/modules'
    module_root.mkdir(parents=True)
    for name, (_, payload, fields) in before.items():
        if name.startswith('lib/modules/'):
            require('/' not in name[len('lib/modules/'):], 'Only flat vendor modules are supported')
            require(stat.S_ISREG(fields[1]), 'Nonregular vendor module record')
            path = module_root / Path(name).name
            path.write_bytes(payload)
            path.chmod(stat.S_IMODE(fields[1]))
    for name in ['vendor_dlkm', 'system_dlkm']:
        trees[name] = a.output / ('original-' + name)
        builder.run([builder.tool('fsck.erofs'), '--extract=' + str(trees[name]), '--no-preserve-owner',
                     '--preserve-perms', images[name]])
    ownership = {name: builder.check_ownership(images[name], trees[name], name)
                 for name in ['vendor_dlkm', 'system_dlkm']}
    original_trees = dict(trees)
    module_reports, verification = assemble_modules(builder, artifact, trees, profile)
    output_images = a.output / 'images'
    output_images.mkdir()
    cpio = a.output / 'vendor.cpio'
    cpio.write_bytes(rebuild_cpio(before, trees['vendor_ramdisk']))
    ramdisk = a.output / 'vendor_ramdisk.lz4'
    builder.run([builder.tool('lz4'), '-l', '-12', cpio, ramdisk])
    params = (artifact / 'inputs/boot-parameters.txt').read_text().split()
    require(params in [['android_arch_task_struct_size=512'], ['android_arch_task_struct_size=512', 'sched_proxy_exec=0']],
            'Unreviewed release boot parameters')
    cmdline = args[args.index('--cmdline') + 1].split()
    firmware = None
    if not a.firmware_package and (artifact / 'firmware/manifest.json').is_file():
        a.firmware_package = artifact / 'firmware'
        a.firmware_manifest_sha256 = read(artifact / 'report.json')['sha256']['firmware/manifest.json']
    native_definition = artifact / 'inputs/nethunter-native-modules.json'
    if native_definition.exists() and read(native_definition).get('wireless_firmware'):
        require(a.firmware_package, 'Native wireless module release requires its matched firmware package')
    if a.firmware_package:
        require(a.firmware_manifest_sha256 and sha(a.firmware_package / 'manifest.json') == a.firmware_manifest_sha256,
                'Firmware manifest identity is required')
        firmware = read(a.firmware_package / 'manifest.json')
        native_definition = artifact / 'inputs/nethunter-native-modules.json'
        if native_definition.exists():
            require(set(read(native_definition)['wireless_firmware']) <= {e['path'] for e in firmware['blobs']},
                    'Required wireless firmware is missing from the package')
        for entry in firmware['blobs']:
            safe_name(entry['path'])
            path = a.firmware_package / 'firmware' / entry['path']
            require(path.stat().st_size == entry['bytes'] and sha(path) == entry['sha256'], 'Firmware blob changed')
            require((a.firmware_package / safe_name(entry['license'])).is_file(), 'Firmware license is missing')
        source = firmware.get('ath_corresponding_source')
        if source:
            require(any(p.is_file() and sha(p) == source['sha256'] for p in (a.firmware_package / 'sources').iterdir()),
                    'Corresponding ATH firmware source is missing')
        existing = [n.split('=', 1)[1] for n in cmdline if n.startswith('firmware_class.path=')]
        require(len(existing) <= 1, 'Duplicate firmware search parameter')
        custom_paths = ['/vendor_dlkm/lib/firmware'] + (existing[0].split(',') if existing else [])
        custom_paths = list(dict.fromkeys(custom_paths))
        require(len(custom_paths) <= 10 and all(p.startswith('/') for p in custom_paths), 'Unsupported firmware search paths')
        params += ['firmware_class.path=' + ','.join(custom_paths)]
    keys = {n.split('=', 1)[0] for n in params}
    cmdline = [n for n in cmdline if n.split('=', 1)[0] not in keys] + params
    option(args, '--kernel', artifact / 'gki/Image')
    option(args, '--cmdline', ' '.join(cmdline))
    builder.run([builder.tool('mkbootimg'), *args, '--output', output_images / 'boot.img'])
    option(vendor_args, '--dtb', artifact / 'devicetrees/dtb.img')
    option(vendor_args, '--vendor_ramdisk_fragment', ramdisk)
    stage = a.output / 'zip'
    stage.mkdir()
    if firmware:
        for directory in ['licenses', 'sources']:
            shutil.copytree(a.firmware_package / directory, stage / 'firmware-notices' / directory)
        shutil.copyfile(a.firmware_package / 'manifest.json', stage / 'firmware-notices/manifest.json')
    config = (vendor / 'bootconfig').read_text()
    old_override = [n for n in config.splitlines() if n.startswith('androidboot.init_rc=')]
    require(len(old_override) <= 1, 'Duplicate init override')
    bootstrap_rows = []
    rollback_bootstrap_rows = []
    rollback_requirements = []
    if 'loaders' in profile:
        loader = profile['loaders']
        source = Path(loader['source']).resolve()
        manifest = read(source / 'manifest.json')
        require(sha(source / 'manifest.json') == loader['sha256'], 'Loader manifest changed')
        require(loader['sha256'] in (SERIAL_MANIFEST_SHA256, SERIAL_PUBLIC_MANIFEST_SHA256),
                'Unreviewed serial loader manifest')
        require(any(i['path'] == '/vendor/etc/selinux/precompiled_sepolicy' and i['sha256'] == SERIAL_POLICY_SHA256
                    for i in profile.get('prerequisite_files', [])),
                'Serial loader installation currently supports only the permission-audited development ROM policy')
        directory = '/metadata/ota/konoha-release-' + a.release_sha256[:16]
        previous_directory = None
        if old_override:
            previous_init = old_override[0].split('=', 1)[1]
            require(previous_init == manifest['device_directory'] + '/init.rc' or
                    re.fullmatch(r'/metadata/ota/konoha-release-[0-9a-f]{16}/init.rc', previous_init),
                    'Unreviewed baseline init override; rollback loaders cannot be inferred')
            previous_directory = str(PurePosixPath(previous_init).parent)
        (stage / 'bootstrap').mkdir()
        for name, entry in manifest['files'].items():
            require(name in ['init.rc', 'services.rc', 'vendor_modprobe.sh', 'system_dlkm_modprobe.sh'], 'Unreviewed bootstrap file')
            require(sha(source / name) == entry['sha256'], 'Bootstrap source changed')
            require(entry['mode'] == (0o755 if name.endswith('.sh') else 0o644) and
                    entry['label'] == ('u:object_r:vendor_modinstall-sh_exec:s0' if name.endswith('.sh') else 'u:object_r:system_file:s0'),
                    'Unreviewed bootstrap file mode or label')
            text = (source / name).read_text().replace(manifest['device_directory'], directory)
            if previous_directory:
                previous = (source / name).read_text().replace(manifest['device_directory'], previous_directory)
                previous_sha = hashlib.sha256(previous.encode()).hexdigest()
                previous_path = previous_directory + '/' + name
                rollback_requirements.append({'path': previous_path, 'sha256': previous_sha})
                rollback_bootstrap_rows.append(f'{previous_path}\t{entry["mode"]:o}\t{entry["label"]}\t{previous_sha}\n')
            target = stage / 'bootstrap' / name
            target.write_text(text)
            target.chmod(entry['mode'])
            bootstrap_rows.append(f'{name}\t{entry["mode"]:o}\t{entry["label"]}\t{sha(target)}\n')
        require(len(bootstrap_rows) == 4, 'Four bootstrap files are required')
        config = '\n'.join(n for n in config.splitlines() if not n.startswith(('androidboot.init_rc=', 'androidboot.konoha_serial_modules=', 'androidboot.load_modules_parallel=')))
        config += '\nandroidboot.load_modules_parallel=false\nandroidboot.init_rc=' + directory + '/init.rc\nandroidboot.konoha_serial_modules=1\n'
        (stage / 'bootstrap-directory').write_text(directory + '\n')
        (stage / 'bootstrap.tsv').write_text(''.join(bootstrap_rows))
    else:
        require(not old_override, 'Baseline init override requires supplied, verified bootstrap loaders')
    (stage / 'rollback-bootstrap.tsv').write_text(''.join(rollback_bootstrap_rows))
    bootconfig = a.output / 'bootconfig'
    bootconfig.write_text(config)
    option(vendor_args, '--vendor_bootconfig', bootconfig)
    builder.run([builder.tool('mkbootimg'), *vendor_args, '--vendor_boot', output_images / 'vendor_boot.img'])
    shutil.copyfile(artifact / 'devicetrees/dtbo.img', output_images / 'dtbo.img')
    for name in ['vendor_dlkm', 'system_dlkm']:
        root = original_trees[name]
        previous = {n: v for n, v in tree_map(root).items() if nonmodules(n)}
        shutil.rmtree(root / 'lib/modules')
        shutil.copytree(trees[name], root / 'lib/modules')
        expected = tree_map(root)
        require({n: v for n, v in expected.items() if nonmodules(n)} == previous, 'Nonmodule filesystem entries changed')
        if name == 'vendor_dlkm' and firmware:
            for entry in firmware['blobs']:
                target = root / 'lib/firmware' / entry['path']
                require(not target.exists() or (target.is_file() and sha(target) == entry['sha256']),
                        'Firmware overlay conflicts with baseline contents')
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(a.firmware_package / 'firmware' / entry['path'], target)
                target.chmod(0o644)
            expected = tree_map(root)
        builder.run([builder.tool('mkfs.erofs'), '-zlz4hc,level=12', '-C262144', '-Efragments,dedupe,ztailpacking',
                     '-T' + str(EPOCH), '--all-root', '--file-contexts=' + str(a.file_contexts.resolve()),
                     '--mount-point=/' + name, output_images / (name + '.img'), root])
        checked = a.output / ('checked-' + name)
        builder.run([builder.tool('fsck.erofs'), '--extract=' + str(checked), '--no-preserve-owner', '--preserve-perms', output_images / (name + '.img')])
        require(tree_map(checked) == expected, 'Rebuilt EROFS differs from expected files/modes')
    layouts = {n: builder.footer(images[n], output_images / (n + '.img'), n) for n in PARTITIONS}
    checked_boot_args, checked_boot = builder.unpack(output_images / 'boot.img', 'checked-boot')
    require(sha(checked_boot / 'kernel') == sha(artifact / 'gki/Image') and
            (checked_boot / 'ramdisk').read_bytes() == (boot / 'ramdisk').read_bytes(), 'Boot kernel/ramdisk readback mismatch')
    require(checked_boot_args[checked_boot_args.index('--cmdline') + 1] == ' '.join(cmdline), 'Boot cmdline mismatch')
    _, checked_vendor = builder.unpack(output_images / 'vendor_boot.img', 'checked-vendor')
    require(sha(checked_vendor / 'dtb') == sha(artifact / 'devicetrees/dtb.img') and
            sha(checked_vendor / 'vendor_ramdisk00') == sha(ramdisk) and
            (checked_vendor / 'bootconfig').read_text() == config, 'Vendor boot readback mismatch')
    payload_rows, baseline_rows = [], []
    for name in PARTITIONS:
        path = output_images / (name + '.img')
        shutil.copyfile(path, stage / path.name)
        payload_rows.append(f'{name}\t{path.stat().st_size}\t{sha(path)}\n')
        baseline_rows.append(f'{name}\t{images[name].stat().st_size}\t{sha(images[name])}\n')
    for name in ['init_boot', 'vbmeta']:
        baseline_rows.append(f'{name}\t{images[name].stat().st_size}\t{sha(images[name])}\n')
    (stage / 'payloads.tsv').write_text(''.join(payload_rows))
    (stage / 'baseline.tsv').write_text(''.join(baseline_rows))
    requirements = list(profile.get('prerequisite_files', [])) + rollback_requirements
    for item in requirements:
        require(re.fullmatch(r'/[A-Za-z0-9_./-]+', item['path']) and '..' not in PurePosixPath(item['path']).parts and
                re.fullmatch('[0-9a-f]{64}', item['sha256']), 'Invalid prerequisite file')
    (stage / 'prerequisites.tsv').write_text(''.join(i['path'] + '\t' + i['sha256'] + '\n' for i in requirements))
    (stage / 'tools').mkdir()
    shutil.copyfile(a.busybox, stage / 'tools/busybox')
    (stage / 'tools/busybox').chmod(0o755)
    shutil.copyfile(HERE / 'tools/recovery-guard', stage / 'tools/recovery-guard')
    (stage / 'tools/recovery-guard').chmod(0o755)
    shutil.copyfile(HERE / 'tools/recovery-guard-build.json', stage / 'tools/recovery-guard-build.json')
    shutil.copytree(a.busybox_notices, stage / 'busybox-notices')
    (stage / 'installer-source').mkdir()
    for name in source_names:
        if name.startswith('tools/'):
            continue
        shutil.copyfile(HERE / name, stage / 'installer-source' / name)
    shutil.copyfile(HERE / 'COPYING', stage / 'installer-source/COPYING')
    (stage / 'installer-source/source-sha256.json').write_text(json.dumps(source_hashes, indent=2) + '\n')
    shutil.copyfile(HERE / 'install.sh', stage / 'install.sh')
    meta = stage / 'META-INF/com/google/android'
    meta.mkdir(parents=True)
    shutil.copyfile(HERE / 'update-binary', meta / 'update-binary')
    (meta / 'update-binary').chmod(0o755)
    (stage / 'install.sh').chmod(0o755)
    (meta / 'updater-script').write_text('# Konoha recovery installer\n')
    require(source_hashes == {name: sha(HERE / name) for name in source_names},
            'Generator/installer source changed during generation; use a fresh output and stable source')
    report = {'status': 'PASS: offline images and ZIP generated; recovery runtime untested', 'device': 'onyx',
              'kernel_release': (artifact / 'gki/kernel.release').read_text().strip(), 'release_sha256': a.release_sha256,
              'profile_sha256': sha(a.profile), 'file_contexts_sha256': sha(a.file_contexts),
              'generator_sources': source_hashes,
              'recovery_busybox_sha256': sha(a.busybox),
              'recovery_guard_sha256': sha(HERE / 'tools/recovery-guard'),
              'busybox_notices_sha256': sha(a.busybox_notices / 'manifest.json'),
              'baseline': {name: {'size': item['size'], 'sha256': item['sha256']}
                           for name, item in profile['images'].items()},
              'host_tools_sha256': {name: sha(builder.tools / name) for name in
                                   ['mkbootimg', 'unpack_bootimg', 'mkfs.erofs', 'fsck.erofs', 'dump.erofs', 'avbtool', 'lz4']},
              'module_trees': module_reports, 'module_verification': verification, 'layouts': layouts,
              'soft_dependency_metadata': builder.soft_dependency_metadata,
              'baseline_erofs_ownership': ownership,
              'firmware_manifest_sha256': a.firmware_manifest_sha256,
              'images': {n: {'size': (output_images / (n + '.img')).stat().st_size,
                             'sha256': sha(output_images / (n + '.img'))} for n in PARTITIONS},
              'commands': builder.commands,
              'limits': ['Exact baseline image hashes are mandatory at install; no arbitrary-ROM migration.',
                         'Existing AVB verification and verity must already be disabled; no signature bypass.',
                         'Only header v4, one LZ4 newc ramdisk, flat modules and root-owned EROFS DLKM supported.',
                         'No dynamic partition resize or super/vbmeta/recovery/init_boot write.',
                         'Selected module layout (or full catalog) is rebound to exact release bytes; baseline load requests are retained.',
                         'Recovery needs /system/bin/sh, ZIP extraction, blockdev and mounted metadata; the bundled static guard uses real kernel ioctls and holds the AOSP directory lock.',
                         'OTA state file and snapshots directory must exist and be clean; missing, unknown, malformed or progressing states fail closed.',
                         'SELinux chcon and exact policy prerequisite are required for the optional serial bootstrap.',
                         'If a partition write/readback fails, restore all matched baseline images before reboot.']}
    replacements = {str(a.output.resolve()): '<output>', str(a.output): '<output>',
                    str(builder.tools): '<android-host-tools>', str(HERE): '<installer-source>',
                    str(a.file_contexts): '<rom-file-contexts>', str(a.busybox): '<busybox-binary>'}
    replacements.update({str(path): '<baseline-images>/' + name + '.img' for name, path in images.items()})
    if profile.get('module_layout'):
        replacements[str(Path(profile['module_layout']['source']).resolve())] = '<module-layout>'
    if a.firmware_package:
        replacements[str(a.firmware_package.resolve())] = '<firmware-package>'
    report = sanitized(report, replacements)
    (stage / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    (a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    make_zip(stage, a.output / 'konoha-onyx-matched.zip')
    print(a.output / 'konoha-onyx-matched.zip')


if __name__ == '__main__':
    main()
