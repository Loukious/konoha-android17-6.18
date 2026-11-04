#!/usr/bin/env python3
"""Execute the actual installer against regular files and mocked recovery services."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PARTS = ['system_dlkm', 'vendor_dlkm', 'dtbo', 'vendor_boot', 'boot']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


STUB = r'''#!/usr/bin/env python3
import json,os,subprocess,sys
from pathlib import Path
r=Path(os.environ['TEST_ROOT']);case=os.environ['TEST_CASE'];slot=os.environ['TEST_SLOT'];name=Path(sys.argv[0]).name;args=sys.argv[1:]
if name=='getprop':
 v={'ro.product.device':'other' if case=='wrong_device' else 'onyx','ro.vendor.product.device':'other' if case=='wrong_device' else 'onyx','ro.boot.flash.locked':'1' if case=='locked' else '0','ro.boot.slot_suffix':'' if case=='bad_slot' else slot}
 if args[0]=='ro.boot.slot_suffix' and case=='slot_changed':
  p=r/'slot-calls';n=int(p.read_text())+1 if p.exists() else 1;p.write_text(str(n));v[args[0]]=slot if n==1 else '_b'
 print(v.get(args[0],''));sys.exit(0)
if name=='ps':print('zygote64' if case=='android_running' else 'recovery');sys.exit(0)
if name=='recovery-guard':
 if args[0]=='snapshot':
  if case in ['active_snapshot','leftover_snapshot','snapshot_error']:sys.exit(1)
  if case=='snapshot_changed_after_write' and (r/'writes.log').exists():sys.exit(1)
  sys.exit(0)
 part=Path(args[2] if args[0]=='dm' else args[1]).name
 if args[0]=='dm' and case in ['nonlinear','guard_unmounted_alias']:sys.exit(1)
 p=r/(part+'-guard-calls');n=int(p.read_text())+1 if p.exists() else 1;p.write_text(str(n))
 event=1 if case=='mapping_changed' and n>=2 and part.startswith('system_dlkm') else 0
 if case=='mapping_drift_after_write' and n==4 and part.startswith('system_dlkm'):event=1
 print(part+' identity event='+str(event));sys.exit(0)
if name=='blockdev':
 if args[0]=='--getsize64':print(Path(args[-1]).stat().st_size)
 sys.exit(0)
if name=='chcon':
 if case=='label_failure':sys.exit(1)
 p=r/'labels.json';v=json.loads(p.read_text()) if p.exists() else {};v[args[-1]]=args[0];p.write_text(json.dumps(v));sys.exit(0)
if name=='ls' and args[0]=='-Zd':
 p=r/'labels.json';v=json.loads(p.read_text()) if p.exists() else {};print(v.get(args[-1],'unlabeled')+' '+args[-1]);sys.exit(0)
if name=='busybox':
 command=args[0];args=args[1:]
 if command=='dd':
  source=next((a[3:] for a in args if a.startswith('if=')),None);target=next((a[3:] for a in args if a.startswith('of=')),None)
  if target and target.startswith(str(r/'dev')):
   with (r/'writes.log').open('a') as f:f.write(Path(target).name+'\n')
   initial=str(Path(source).resolve()).startswith(str(r/'package'))
   if initial and case=='write_failure' and 'vendor_dlkm' in target:sys.exit(1)
   if initial and case=='readback_corrupt' and 'vendor_boot' in target:
    Path(target).write_bytes(Path(source).read_bytes()[:-3]+b'BAD');sys.exit(0)
  sys.exit(subprocess.run(['/usr/bin/dd',*args]).returncode)
 sys.exit(subprocess.run([shutil_path(command),*args]).returncode)
sys.exit(subprocess.run(['/usr/bin/'+name,*args]).returncode)
'''.replace("sys.exit(subprocess.run([shutil_path(command),*args]).returncode)",
            "sys.exit(subprocess.run([__import__('shutil').which(command,path='/usr/bin:/bin'),*args]).returncode)")


def execute(case, temporary):
    root = temporary / case
    package = root / 'package'
    package.mkdir(parents=True)
    bindir = root / 'bin'
    bindir.mkdir()
    metadata = root / 'metadata'
    (metadata / 'ota').mkdir(parents=True)
    (root / 'proc').mkdir()
    (root / 'proc/mounts').write_text(f'none {metadata} ext4 rw 0 0\n')
    slot = '_b' if case == 'active_b' else '_a'
    mapper, byname = root / 'dev/block/mapper', root / 'dev/block/by-name'
    mapper.mkdir(parents=True)
    byname.mkdir(parents=True)
    (byname / 'super').write_bytes(b'PHYSICAL-SUPER')
    rows = []
    for suffix in ['_a', '_b']:
        for name in PARTS + ['init_boot', 'vbmeta']:
            path = (mapper if name.endswith('dlkm') else byname) / (name + suffix)
            content = (name + suffix + '-BASELINE').encode().ljust(4096, b'x')
            if name == 'vbmeta':
                content = bytearray(content)
                content[:4] = b'AVB0'
                struct.pack_into('>I', content, 120, 0 if case == 'bad_avb' else 3)
            path.write_bytes(content)
            if suffix == slot:
                rows.append(f'{name}\t4096\t{sha(path)}\n')
    (package / 'baseline.tsv').write_text(''.join(rows))
    payloads = []
    for name in PARTS:
        p = package / (name + '.img')
        p.write_bytes((name + '-NEW').encode().ljust(4096, b'0'))
        payloads.append(f'{name}\t4096\t{sha(p)}\n')
    (package / 'payloads.tsv').write_text(''.join(payloads))
    prerequisite = root / 'policy.bin'
    prerequisite.write_bytes(b'PINNED-POLICY')
    (package / 'prerequisites.tsv').write_text(f'{prerequisite}\t{sha(prerequisite)}\n')
    (package / 'rollback-bootstrap.tsv').write_text('')
    if case in ['bootstrap', 'label_failure']:
        (package / 'bootstrap').mkdir()
        loader_rows = []
        for name in ['init.rc', 'services.rc', 'vendor_modprobe.sh', 'system_dlkm_modprobe.sh']:
            p = package / 'bootstrap' / name
            p.write_text('TEST-LOADER\n')
            loader_rows.append(f'{name}\t644\tu:object_r:system_file:s0\t{sha(p)}\n')
        (package / 'bootstrap.tsv').write_text(''.join(loader_rows))
        (package / 'bootstrap-directory').write_text(str(metadata / 'ota/konoha-release-0123456789abcdef') + '\n')
    source = (HERE / 'install.sh').read_text()
    source = source.replace('/dev/block', str(root / 'dev/block'))
    source = source.replace('/dev/*)', str(root / 'dev') + '/*)')
    source = source.replace('/metadata', str(metadata)).replace('/proc/mounts', str(root / 'proc/mounts'))
    source = source.replace('is_block() { [ -b "$1" ]; }', 'is_block() { [ -f "$1" ]; }')
    (package / 'install.sh').write_text(source)
    for name in ['busybox', 'getprop', 'ps', 'blockdev', 'chcon', 'ls']:
        p = bindir / name
        p.write_text(STUB)
        p.chmod(0o755)
    (package / 'tools').mkdir()
    (package / 'tools/recovery-guard').write_text(STUB)
    (package / 'tools/recovery-guard').chmod(0o755)
    (package / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + str(p.relative_to(package)) + '\n'
                                            for p in package.rglob('*') if p.is_file()))
    baseline = {path: sha(path) for path in (root / 'dev').rglob('*') if path.is_file()}
    if case == 'wrong_baseline': (byname / ('boot' + slot)).write_bytes(b'wrong'.ljust(4096, b'x'))
    if case == 'wrong_size': (byname / ('boot' + slot)).write_bytes(b'wrong-size')
    if case == 'missing_partition': (byname / ('vendor_boot' + slot)).unlink()
    if case == 'tampered_payload': (package / 'boot.img').write_bytes(b'TAMPERED'.ljust(4096, b'0'))
    if case == 'wrong_policy': prerequisite.write_bytes(b'OTHER-POLICY')
    if case == 'mounted_dlkm':
        with (root / 'proc/mounts').open('a') as f:f.write(f'{mapper}/vendor_dlkm{slot} /vendor_dlkm erofs ro 0 0\n')
    if case == 'no_metadata': (root / 'proc/mounts').write_text('')
    if case == 'missing_super': (byname / 'super').unlink()
    if case == 'duplicate_manifest':
        (package / 'baseline.tsv').write_text(''.join(rows + [rows[0]]))
        (package / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + str(p.relative_to(package)) + '\n'
                                                for p in package.rglob('*') if p.is_file() and p.name != 'SHA256SUMS'))
    run = subprocess.run(['bash', str(package / 'install.sh')], cwd=package, capture_output=True,
                         text=True, timeout=45, env=dict(os.environ, PATH=str(bindir) + ':/usr/bin:/bin',
                         TEST_ROOT=str(root), TEST_CASE=case, TEST_SLOT=slot, KONOHA_PACKAGE=str(package),
                         KONOHA_BUSYBOX=str(bindir / 'busybox'), OUTFD='1',
                         KONOHA_GUARD_ACTIVE='0' if case=='missing_guard_lock' else '1'))
    writes = (root / 'writes.log').read_text().splitlines() if (root / 'writes.log').exists() else []
    positive = case in ['active_a', 'active_b', 'bootstrap']
    if positive:
        assert run.returncode == 0, (case, run.stdout, run.stderr)
        assert writes == [n + slot for n in PARTS], (case, writes)
        for name in PARTS:
            p = (mapper if name.endswith('dlkm') else byname) / (name + slot)
            assert p.read_bytes() == (package / (name + '.img')).read_bytes()
    else:
        assert run.returncode != 0, (case, run.stdout, run.stderr)
        if case in ['write_failure', 'readback_corrupt', 'mapping_drift_after_write', 'snapshot_changed_after_write']:
            assert 'Baseline restored and verified' in run.stdout, (case, run.stdout, run.stderr)
            assert all(sha(p) == h for p, h in baseline.items()), (case, writes)
            assert writes[-5:] == [n + slot for n in PARTS]
        else:
            assert writes == [], (case, writes, run.stdout, run.stderr)
    assert all(sha(p) == h for p, h in baseline.items() if p.name.endswith('_a' if slot == '_b' else '_b'))
    return {'case': case, 'status': 'PASS', 'partition_writes': writes, 'inactive_slot_unchanged': True}


if __name__ == '__main__':
    cases = ['active_a', 'active_b', 'bootstrap', 'wrong_device', 'locked', 'bad_slot',
             'android_running', 'active_snapshot', 'leftover_snapshot', 'snapshot_error',
             'nonlinear', 'wrong_baseline', 'wrong_size', 'missing_partition', 'tampered_payload',
             'wrong_policy', 'mounted_dlkm', 'no_metadata', 'duplicate_manifest', 'bad_avb',
             'slot_changed', 'label_failure', 'write_failure', 'readback_corrupt']
    cases += ['missing_guard_lock', 'missing_super', 'guard_unmounted_alias',
              'mapping_changed', 'mapping_drift_after_write', 'snapshot_changed_after_write']
    with tempfile.TemporaryDirectory(prefix='konoha-installer-test-') as path:
        results = [execute(case, Path(path)) for case in cases]
    report = {'status': 'PASS', 'installer_sha256': sha(HERE / 'install.sh'), 'cases': results,
              'limits': 'Actual shell source; regular-file targets, mocked guard, properties, SELinux labels and blockdev. Separate native guard tests exercise real host syscalls/parsers; AArch64 BusyBox/recovery/partition behavior needs runtime validation; no phone accessed.'}
    (HERE / 'installer-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(results)} actual-installer scenarios, both slots, bootstrap labels and full rollback')
