"""Exercise the production shell flow against files, never actual block devices."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PARTS = ('system_dlkm', 'vendor_dlkm', 'dtbo', 'vendor_boot', 'recovery', 'boot')

FAKE_TOOL = r'''#!/usr/bin/env python3
import hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(os.environ['FIXTURE_ROOT'])
state_file=root/'state.json'
state=json.loads(state_file.read_text())
mode=os.environ.get('FIXTURE_MODE','success')
def save(): state_file.write_text(json.dumps(state))
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
tool=Path(sys.argv[0]).name
args=sys.argv[1:]
if tool=='getprop':
    props={'ro.product.device':'wrong' if mode=='wrong_device' else 'onyx',
           'ro.vendor.product.device':'wrong' if mode=='wrong_device' else 'onyx',
           'ro.boot.flash.locked':'0','ro.boot.slot_suffix':'_a'}
    print(props.get(args[0],''));sys.exit(0)
if tool=='recovery-guard':
    if args[0]=='snapshot': sys.exit(1 if mode=='snapshot' else 0)
    if mode=='mapping_change' and state.get('prepared'): sys.exit(1)
    print('stable mapping '+args[1]);sys.exit(0)
if tool=='onyx-recovery':
    if args[0]=='check-layout':
        if mode=='shared_extents': print('shared inactive slot extent');sys.exit(1)
        print('layout baseline');sys.exit(0)
    if args[0]!='prepare': sys.exit(2)
    if mode=='prepare_failure': print('incompatible ROM module');sys.exit(1)
    original,out=Path(args[2]),Path(args[3]);out.mkdir()
    rows=[]
    for part in ('system_dlkm','vendor_dlkm','dtbo','vendor_boot','recovery','boot'):
        old=(original/(part+'.img')).read_bytes()
        data=('updated '+part).encode().ljust(len(old),b'!')
        target=out/(part+'.img');target.write_bytes(data)
        if mode!='missing_image' or part!='recovery':
            rows.append(f'{part}\t{len(data)}\t{sha(target)}\t{sha(original/(part+".img"))}\n')
    (out/'prepared.tsv').write_text(''.join(rows))
    (out/'prepared.json').write_text('{}')
    if mode=='corrupt_prepared': (out/'boot.img').write_bytes(b'changed')
    state['prepared']=True;save();sys.exit(0)
if tool!='busybox': sys.exit(2)
command,args=args[0],args[1:]
if command=='sync': sys.exit(0)
if command=='df':
    available=0 if mode=='space' else 8_000_000
    print('Filesystem 1024-blocks Used Available Capacity Mounted-on')
    print(f'fixture 9000000 1 {available} 1% {args[-1]}');sys.exit(0)
if command=='blockdev':
    flag,path=args
    if flag=='--getsize64': print(Path(path).stat().st_size)
    elif flag=='--getro': print(state['ro'].get(path,1))
    elif flag in ('--setrw','--setro'): state['ro'][path]=int(flag=='--setro');save()
    else: sys.exit(2)
    sys.exit(0)
if command=='dd':
    options=dict(v.split('=',1) for v in args if '=' in v)
    source,target=Path(options['if']),Path(options['of'])
    data=source.read_bytes()
    writing=target.parent==root/'devices'
    updating=writing and 'prepared' in source.parts
    if writing: state['writes'].append({'partition':target.name,'update':updating});save()
    if updating and target.name=='dtbo_a' and mode in ('write_failure','readback_failure') and not state.get('failed_once'):
        state['failed_once']=True;save();target.write_bytes(b'partial'.ljust(len(data),b'?'))
        sys.exit(1 if mode=='write_failure' else 0)
    if not writing and mode=='backup_failure' and target.name=='vendor_boot.img': data=b'bad backup'
    target.write_bytes(data);sys.exit(0)
os.execvp(command,[command,*args])
'''


class CompleteInstaller(unittest.TestCase):
    def run_fixture(self, mode):
        with tempfile.TemporaryDirectory(prefix='onyx-installer-test-') as temp:
            root = Path(temp)
            package, work = root/'package', root/'work'
            for path in (package/'tools', work, root/'bin', root/'devices', root/'dev/block/by-name', root/'dev/block/mapper', root/'data'):
                path.mkdir(parents=True, exist_ok=True)
            baseline = {}
            for part in (*PARTS, 'vbmeta'):
                data = ('original '+part).encode().ljust(4096, b'.')
                path = root/'devices'/(part+'_a')
                path.write_bytes(data)
                baseline[path] = data
                folder = 'mapper' if part.endswith('dlkm') else 'by-name'
                (root/'dev/block'/folder/path.name).symlink_to(path)
            (root/'devices/super').write_bytes(b'super metadata')
            (root/'dev/block/by-name/super').symlink_to(root/'devices/super')
            (root/'mounts').write_text(f'/dev/fake {root}/data ext4 rw 0 0\n')
            (root/'state.json').write_text(json.dumps({'writes': [], 'ro': {}}))
            for name in ('busybox', 'recovery-guard', 'onyx-recovery'):
                p=package/'tools'/name;p.write_text(FAKE_TOOL);p.chmod(0o755)
            p=root/'bin/getprop';p.write_text(FAKE_TOOL);p.chmod(0o755)
            source=(ROOT/'ci/recovery/install.sh').read_text()
            # The only device-type substitution lives in this test copy. The
            # shipped installer has no environment-controlled test bypass.
            old='is_block() { [ -b "$1" ]; }'
            self.assertEqual(source.count(old),1)
            source=source.replace(old,'is_block() { [ -f "$1" ]; }')
            source=source.replace('/dev/block',str(root/'dev/block')).replace('case "$source" in /dev/*)','case "$source" in '+str(root/'dev')+'/*)')
            source=source.replace('/proc/mounts',str(root/'mounts'))
            for name in ('data','cache','metadata'):
                source=source.replace('/'+name,str(root/name))
            script=package/'install.sh';script.write_text(source)
            (package/'kernel.json').write_text('{}')
            sums=''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(package).as_posix()+'\n' for p in sorted(package.rglob('*')) if p.is_file())
            (package/'SHA256SUMS').write_text(sums)
            env=dict(os.environ,ONYX_PACKAGE=str(package),ONYX_WORK=str(work),ONYX_BUSYBOX=str(package/'tools/busybox'),ONYX_GUARD_ACTIVE='1',OUTFD='1',FIXTURE_ROOT=str(root),FIXTURE_MODE=mode)
            env['PATH']=str(root/'bin')+':'+env['PATH']
            result=subprocess.run(['bash',str(script)],env=env,text=True,capture_output=True,timeout=45)
            state=json.loads((root/'state.json').read_text())
            if mode=='success':
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertEqual([r['partition'] for r in state['writes']],[p+'_a' for p in PARTS])
                for path,data in baseline.items():
                    if path.name=='vbmeta_a': self.assertEqual(path.read_bytes(),data)
                    else: self.assertTrue(path.read_bytes().startswith(b'updated '))
            else:
                self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
                for path,data in baseline.items(): self.assertEqual(path.read_bytes(),data,result.stdout+result.stderr)
                if mode in ('write_failure','readback_failure'):
                    restored=[r['partition'] for r in state['writes'] if not r['update']]
                    self.assertEqual(restored,[p+'_a' for p in PARTS],result.stdout+result.stderr)
                    self.assertIn('Original images restored and verified',result.stdout)
                else: self.assertEqual(state['writes'],[],result.stdout+result.stderr)
            self.assertTrue(all(value==1 for value in state['ro'].values()))

    def test_all_six_images_and_readback(self): self.run_fixture('success')

    def test_write_and_readback_failures_restore_the_complete_baseline(self):
        for mode in ('write_failure','readback_failure'):
            with self.subTest(mode=mode): self.run_fixture(mode)

    def test_preflight_failures_do_not_write_partitions(self):
        for mode in ('wrong_device','snapshot','space','shared_extents','prepare_failure','missing_image','corrupt_prepared','backup_failure','mapping_change'):
            with self.subTest(mode=mode): self.run_fixture(mode)


if __name__=='__main__': unittest.main()
