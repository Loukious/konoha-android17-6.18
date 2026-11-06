#!/usr/bin/env python3
"""Generate ROM userspace headers from the synced Onyx GKI and module sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(source, output):
    source, output = source.resolve(), output.resolve()
    if source == output or source.is_relative_to(output) or output.is_relative_to(source):
        raise ValueError('Header output must be outside the synced source')
    destination = output
    env = dict(os.environ)
    env['PATH'] = '/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:' + env.get('PATH', '')
    common = source / 'common'
    if not (common / 'scripts/headers_install.sh').is_file():
        raise ValueError('Sync the Onyx 6.18 source project before building headers')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='onyx-headers-', dir=output) as temp:
        work = Path(temp)
        output = work / 'export'
        output.mkdir()
        # Out-of-tree headers_install does not change the synced source checkout.
        subprocess.run(['/usr/bin/make', '-C', str(common), 'O=' + str(work / 'objects'),
            'ARCH=arm64', 'HOSTCC=/usr/bin/cc', 'INSTALL_HDR_PATH=' + str(output / 'usr'),
            'headers_install'], env=env, check=True)
        (work / 'scripts').mkdir()
        subprocess.run(['/usr/bin/cc', '-O2', str(common / 'scripts/unifdef.c'),
                        '-o', str(work / 'scripts/unifdef')], env=env, check=True)
        env['objtree'] = str(work)
        providers, inputs = {}, {}

        def sanitize(path, destination):
            destination.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['/bin/sh', str(common / 'scripts/headers_install.sh'),
                            str(path), str(destination)], env=env, check=True)

        for path in sorted((source / 'modules').rglob('*.h')):
            relative = path.relative_to(source).as_posix()
            if '/include/uapi/' not in relative:
                continue
            name = relative.split('/include/uapi/', 1)[1]
            aliases = [name]
            if name.split('/')[0] in {'audio', 'camera', 'display', 'eva', 'synx', 'vidc', 'touch', 'sync_fence'}:
                aliases.append(name.split('/', 1)[1])
            cleaned = work / 'sanitized.h'
            sanitize(path, cleaned)
            data = cleaned.read_bytes()
            for alias in aliases:
                dest = output / 'usr/include' / alias
                if alias in providers and dest.read_bytes() != data:
                    if alias == 'linux/qseecom.h' and providers[alias] == 'modules/include/uapi/linux/qseecom.h':
                        continue
                    raise ValueError('Conflicting module UAPI: ' + alias)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
                providers[alias] = relative
            inputs[relative] = digest(path)

        compatibility = json.loads((HERE / 'legacy-uapi/provenance.json').read_text())
        pins = json.loads((source / 'port/sources.json').read_text())
        pin = next(p for p in pins['baseline'] if p['rom_path'] == 'kernel/xiaomi/sm8735')
        if pin['revision'] != compatibility['source']['revision']:
            raise ValueError('ION compatibility header baseline must be reviewed for the new source pin')
        for name, expected in compatibility['sha256'].items():
            path = HERE / 'legacy-uapi' / name
            if digest(path) != expected:
                raise ValueError('Compatibility header changed: ' + name)
            sanitize(path, output / 'usr/include/linux' / name)
        for name in ('linux/types.h', 'linux/msm_ipa.h', 'linux/ion.h', 'linux/msm_ion.h',
                     'linux/msm_ion_ids.h', 'asm/signal.h', 'linux/eventpoll.h'):
            if not (output / 'usr/include' / name).is_file():
                raise ValueError('Required generated UAPI header missing: ' + name)
        (output / 'onyx-headers.json').write_text(json.dumps({
            'source': str(source), 'module_inputs': inputs, 'providers': providers,
            'compatibility': compatibility,
        }, indent=2) + '\n')
        old = work / 'previous-usr'
        if (destination / 'usr').exists():
            (destination / 'usr').rename(old)
        try:
            (output / 'usr').rename(destination / 'usr')
        except BaseException:
            if old.exists():
                old.rename(destination / 'usr')
            raise
        (output / 'onyx-headers.json').replace(destination / 'onyx-headers.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
