#!/usr/bin/env python3
"""Build the synced Onyx port and export matching inputs for Android packaging."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def read(path):
    return json.loads(path.read_text())


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def recipe_key(source):
    names = ('sources.json', 'dependency-lock.xml', 'onyx-port.xml')
    return hashlib.sha256(b''.join((source / 'port' / n).read_bytes() for n in names)).hexdigest()[:20]


def source_state(source):
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    # The replay checks below compare every source file, executable bit and symlink.
    return {'revision': revision, 'recipe': recipe_key(source),
            'common_manifest': digest(source / 'port/common-source-hashes.json'),
            'module_manifest': digest(source / 'port/module-source-hashes.json')}


def outside_source(source, *outputs):
    source = source.resolve()
    for output in outputs:
        output = output.resolve()
        if source == output or source.is_relative_to(output) or output.is_relative_to(source):
            raise ValueError('Build outputs must be outside the synced kernel source')


def package(source, artifact, destination):
    source, artifact, destination = (p.resolve() for p in (source, artifact, destination))
    outside_source(source, destination)
    if artifact == destination or artifact.is_relative_to(destination) or destination.is_relative_to(artifact):
        raise ValueError('Export directory must not overlap build artifacts')
    report = read(artifact / 'report.json')
    if report.get('qdss_diagnostic') is not False:
        raise ValueError('ROM builds require the ordinary kernel recipe')
    for name, expected in report['sha256'].items():
        path = artifact / name
        if Path(name).is_absolute() or '..' in Path(name).parts or digest(path) != expected:
            raise ValueError('Build output hash mismatch: ' + name)
    if read(artifact / 'inputs/sources.json') != read(source / 'port/sources.json'):
        raise ValueError('Build outputs belong to different source inputs')
    for filename in ('common-source-hashes.json', 'module-source-hashes.json', 'modules.json'):
        if read(artifact / 'inputs' / filename) != read(source / 'port' / filename):
            raise ValueError('Build outputs belong to different source: ' + filename)

    # Reuse the checked module-only assembler; no recovery image or installer runs.
    sys.path.insert(0, str(source / 'port'))
    spec = importlib.util.spec_from_file_location('onyx_module_assembler', source / 'port/recovery/generate.py')
    assembler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(assembler)
    layout = read(HERE / 'module-layout.json')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='onyx-export-', dir=destination.parent) as temp:
        work = Path(temp)
        stage = work / 'export'
        stage.mkdir()
        class Builder:
            output = work

            def run(self, command):
                env = dict(os.environ)
                env['PATH'] = '/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:' + env.get('PATH', '')
                return subprocess.check_output([str(arg) for arg in command], env=env)

        builder = Builder()
        trees = {}
        old_names = {name for part in layout['partitions'].values() for name in part['modules']}
        additions = sorted(entry['output'] for entry in read(source / 'port/modules.json')
                           if entry['output'] not in old_names)
        for partition, definition in layout['partitions'].items():
            trees[partition] = work / ('requests-' + partition)
            target = trees[partition] / 'lib/modules'
            target.mkdir(parents=True)
            for name, contents in definition['config'].items():
                if partition == 'vendor_dlkm' and name == 'modules.load':
                    contents += ''.join(n + '\n' for n in additions)
                (target / name).write_text(contents)
        # These are the three audited stale native softdeps, with their evidence
        # descriptions maintained by the source port. Unknown dependencies fail.
        profile = read(source / 'port/recovery/examples/saved-baseline-profile.json')
        reports, verification = assembler.assemble_modules(builder, artifact, trees,
                                                           {'omissions': profile['omissions']})
        all_modules = {p.name for p in (work / 'catalog').glob('*.ko')}
        builtins = {assembler.norm(n) for n in (artifact / 'gki/modules.builtin').read_text().splitlines()}
        missing = {n for n in old_names if n not in all_modules and assembler.norm(n) not in builtins}
        if missing:
            raise ValueError('Previously packaged modules missing from source build: ' + ', '.join(sorted(missing)))
        for path in (work / 'catalog').glob('*.ko'):
            shutil.copyfile(path, stage / path.name)
        for name, original in {
            'Image': 'gki/Image', 'System.map': 'gki/System.map',
            '.config': 'gki/kernel_aarch64_dot_config', 'kernel.release': 'gki/kernel.release',
            'Module.symvers': 'gki/kernel_aarch64_Module.symvers',
            'modules.builtin': 'gki/modules.builtin', 'modules.builtin.modinfo': 'gki/modules.builtin.modinfo',
            'dtb.img': 'devicetrees/dtb.img', 'dtbo.img': 'devicetrees/dtbo.img',
        }.items():
            shutil.copyfile(artifact / original, stage / name)
        for partition, tree in trees.items():
            names = sorted(p.name for p in tree.glob('*.ko'))
            (stage / (partition + '.modules')).write_text(''.join(n + '\n' for n in names))
            for path in tree.glob('modules.*'):
                if path.name in ('modules.load', 'modules.load.recovery', 'modules.blocklist', 'modules.options'):
                    shutil.copyfile(path, stage / (partition + '.' + path.name))
        receipt = {'source': source_state(source), 'kernel_release': report['kernel_release'],
                   'kernelsu_next': report['kernelsu_next'], 'module_count': len(all_modules),
                   'partitions': reports, 'module_verification': verification,
                   'artifact': str(artifact), 'artifact_report_sha256': digest(artifact / 'report.json'),
                   'sha256': {p.name: digest(p) for p in stage.iterdir() if p.is_file()}}
        (stage / 'onyx-source-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
        # destination is an Android build intermediates directory, never a source
        # checkout or an installed phone partition. Replace only after validation.
        previous = work / 'previous-export'
        if destination.exists():
            destination.rename(previous)
        try:
            stage.rename(destination)
        except BaseException:
            if previous.exists():
                previous.rename(destination)
            raise
    return receipt


def build(source, output, workspace_root, jobs, memory_mb):
    source, output, workspace_root = (p.resolve() for p in (source, output, workspace_root))
    outside_source(source, output, workspace_root)
    if output == workspace_root or output.is_relative_to(workspace_root) or workspace_root.is_relative_to(output):
        raise ValueError('Export and build workspace must not overlap')
    workspace_root.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, KONOHA_SOURCE_ROOT=str(source))
    env['PATH'] = '/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:' + env.get('PATH', '')
    if not shutil.which('repo', path=env['PATH']):
        rom = next((p for p in source.parents if (p / '.repo/repo/repo').is_file()), None)
        if rom is None:
            raise ValueError('Android repo launcher is required')
        launchers = workspace_root / 'host-bin'
        launchers.mkdir(exist_ok=True)
        launcher = launchers / 'repo'
        if not launcher.exists():
            launcher.symlink_to(rom / '.repo/repo/repo')
        env['PATH'] = str(launchers) + ':' + env['PATH']
    workspace = workspace_root / recipe_key(source)
    with (workspace_root / 'build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        before = source_state(source)
        commands = [
            [source / 'port/prepare.py', workspace, '--jobs', str(min(jobs, 8))],
            [source / 'port/stage.py', workspace],
            [HERE / 'snapshot.py', workspace / 'onyx'],
            [HERE / 'snapshot_common.py', workspace],
        ]
        for command in commands:
            subprocess.run([sys.executable, *map(str, command)], env=env, check=True)
        previous = set((workspace / 'artifacts').glob('*'))
        subprocess.run([sys.executable, str(source / 'port/build.py'), str(workspace),
                        '--jobs', str(jobs), '--memory-mb', str(memory_mb),
                        '--cache-dir', str(workspace_root / 'kleaf-cache')], env=env, check=True)
        created = set((workspace / 'artifacts').glob('*')) - previous
        if len(created) != 1 or source_state(source) != before:
            raise ValueError('Source changed during build or build artifact selection is ambiguous')
        for command in commands[2:]:
            subprocess.run([sys.executable, *map(str, command)], env=env, check=True)
        return package(source, created.pop(), output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 12))
    parser.add_argument('--memory-mb', type=int, default=20000)
    args = parser.parse_args()
    if args.jobs < 1 or args.memory_mb < 1024:
        parser.error('Invalid kernel build resource limits')
    receipt = build(args.source, args.output, args.workspace, args.jobs, args.memory_mb)
    print(json.dumps({key: receipt[key] for key in ('source', 'kernel_release', 'module_count')}, indent=2))
