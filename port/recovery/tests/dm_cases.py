#!/usr/bin/env python3
"""Independent DM/UAPI review. No block device or phone is opened."""
import copy
import os
import sys
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / 'recovery-guard.c'
ROOT = Path(sys.argv[2]) / 'dm'
ROOT.mkdir()
shutil.copyfile(SOURCE, ROOT / 'recovery-guard.c')

ABI = '''
#include <stddef.h>
#include <linux/dm-ioctl.h>
#include <linux/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include "recovery-guard.c"
#define SAME(a,b) _Static_assert((a)==(b), #a " disagrees with UAPI " #b)
SAME(sizeof(struct dmio),sizeof(struct dm_ioctl));
SAME(sizeof(struct spec),sizeof(struct dm_target_spec));
SAME(sizeof(struct sx),sizeof(struct statx));
SAME(offsetof(struct dmio,version),offsetof(struct dm_ioctl,version));
SAME(offsetof(struct dmio,data_size),offsetof(struct dm_ioctl,data_size));
SAME(offsetof(struct dmio,data_start),offsetof(struct dm_ioctl,data_start));
SAME(offsetof(struct dmio,target_count),offsetof(struct dm_ioctl,target_count));
SAME(offsetof(struct dmio,open_count),offsetof(struct dm_ioctl,open_count));
SAME(offsetof(struct dmio,flags),offsetof(struct dm_ioctl,flags));
SAME(offsetof(struct dmio,event),offsetof(struct dm_ioctl,event_nr));
SAME(offsetof(struct dmio,dev),offsetof(struct dm_ioctl,dev));
SAME(offsetof(struct dmio,name),offsetof(struct dm_ioctl,name));
SAME(offsetof(struct dmio,uuid),offsetof(struct dm_ioctl,uuid));
SAME(offsetof(struct spec,start),offsetof(struct dm_target_spec,sector_start));
SAME(offsetof(struct spec,length),offsetof(struct dm_target_spec,length));
SAME(offsetof(struct spec,status),offsetof(struct dm_target_spec,status));
SAME(offsetof(struct spec,next),offsetof(struct dm_target_spec,next));
SAME(offsetof(struct spec,type),offsetof(struct dm_target_spec,target_type));
SAME(offsetof(struct sx,mode),offsetof(struct statx,stx_mode));
SAME(offsetof(struct sx,size),offsetof(struct statx,stx_size));
SAME(offsetof(struct sx,rdev_major),offsetof(struct statx,stx_rdev_major));
SAME(offsetof(struct sx,rdev_minor),offsetof(struct statx,stx_rdev_minor));
SAME(0xc138fd0c,DM_TABLE_STATUS);
SAME(S_OPEN,__NR_openat);
SAME(S_CLOSE,__NR_close);
SAME(S_READ,__NR_read);
SAME(S_WRITE,__NR_write);
SAME(S_DENTS,__NR_getdents64);
SAME(S_STATX,__NR_statx);
SAME(S_IOCTL,__NR_ioctl);
SAME(S_FLOCK,__NR_flock);
SAME(S_CLONE,__NR_clone);
SAME(S_WAIT,__NR_wait4);
SAME(S_EXEC,__NR_execve);
SAME(S_EXIT,__NR_exit);
'''
(ROOT / 'abi.c').write_text(ABI)
command = [os.environ.get('CLANG', 'clang'),'-fuse-ld=lld','-nostdlib','-static','-ffreestanding','-fno-builtin',
           '-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
           '-Os','-Wall','-Wextra','-Werror']
subprocess.run(command + [str(ROOT / 'abi.c'),'-o',str(ROOT / 'abi')],check=True,capture_output=True)
subprocess.run(command + ['-DGUARD_TEST',str(ROOT / 'recovery-guard.c'),'-o',str(ROOT / 'guard-test')],
               check=True,capture_output=True)

def table(specs=None, **changes):
    if specs is None:
        specs=[dict(start=0,length=8,status=0,type=b'linear',text=b'8:1 128')]
    fields=dict(version=(4,0,0),flags=(1<<4)|(1<<5),event=71,dev=(253<<8)|7,
                name=b'vendor_dlkm_a',uuid=b'ANDROID:vendor_dlkm_a')
    fields.update(changes)
    payload=bytearray()
    ends=[]
    for spec in specs:
        text=spec['text']+b'\0'
        used=len(payload)+40+len(text)
        next_offset=(used+7)&~7
        payload.extend(struct.pack('<QQiI16s',spec['start'],spec['length'],spec['status'],
                                   spec.get('next',next_offset),spec['type'])+text)
        payload.extend(bytes(next_offset-len(payload)))
        ends.append(used)
    data_size=312+ends[-1] if ends else 312
    head=struct.pack('<3I3Ii3IQ128s129s7s',*fields['version'],fields.get('data_size',data_size),
                     fields.get('data_start',312),fields.get('target_count',len(specs)),0,
                     fields['flags'],fields['event'],0,fields['dev'],fields['name'],fields['uuid'],b'')
    return head+payload

cases=[]
def check(name,data,accept):
    path=ROOT/(name+'.bin')
    path.write_bytes(data)
    p=subprocess.run([str(ROOT/'guard-test'),'test-table',str(path)],capture_output=True,text=True,timeout=2)
    ok=(p.returncode==0)==accept
    cases.append(dict(name=name,expected='accept' if accept else 'reject',status='PASS' if ok else 'FAIL',
                      returncode=p.returncode,stdout=p.stdout))
    assert ok,(name,p.returncode,p.stdout)
    return p.stdout

base=table()
fingerprint=check('valid_one_extent',base,True)
multi=[dict(start=0,length=3,status=0,type=b'linear',text=b'8:1 128'),
       dict(start=3,length=5,status=0,type=b'linear',text=b'8:1 300')]
check('valid_multi_extent',table(multi),True)
check('valid_zero_super_offset',table([dict(start=0,length=8,status=0,type=b'linear',text=b'8:1 0')]),True)
for bit,name in [(1,'suspended'),(6,'inactive_table'),(8,'buffer_full'),(12,'query_inactive'),
                 (17,'deferred_remove'),(18,'internal_suspend')]:
    check(name,table(flags=(1<<4)|(1<<5)|(1<<bit)),False)
for bit,name in [(4,'not_table_status'),(5,'no_active_table')]:
    check(name,table(flags=((1<<4)|(1<<5))&~(1<<bit)),False)
for kw,name in [(dict(version=(3,0,0)),'version'),(dict(data_start=304),'data_start'),
                (dict(data_size=65537),'oversize_reply'),(dict(data_size=311),'short_data'),
                (dict(target_count=0),'zero_count'),(dict(target_count=1025),'oversize_count'),
                (dict(name=b'other'),'wrong_name'),(dict(name=b'x'*128),'unterminated_name'),
                (dict(uuid=b'x'*129),'unterminated_uuid'),(dict(dev=(253<<8)|8),'wrong_identity')]:
    check(name,table(**kw),False)
for value,name in [(b'snapshot','snapshot'),(b'snapshot-merge','snapshot_merge'),(b'user','user'),
                   (b'error','error'),(b'x'*16,'unterminated_type')]:
    specs=copy.deepcopy(multi);specs[0]['type']=value
    check(name,table(specs),False)
for change,name in [(dict(start=1),'start_gap'),(dict(length=0),'zero_length'),
                    (dict(length=9),'beyond_target_size'),(dict(status=-1),'status_error'),
                    (dict(next=40),'next_header_only'),(dict(next=49),'unaligned_next'),
                    (dict(next=65536),'out_of_bounds_next')]:
    specs=copy.deepcopy(multi);specs[0].update(change)
    check(name,table(specs),False)
for value,name in [(b'253:1 128','dm_backing'),(b'8:2 128','wrong_super_minor'),
                   (b'8:1 2048','extent_beyond_super'),(b'8:1 2041','extent_overflow'),
                   (b'8:1 18446744073709551616','integer_overflow'),(b'8:1 -1','negative_offset'),
                   (b'8:1 128 junk','trailing_parameters'),(b'8:1\t128','parameter_tab')]:
    specs=[dict(start=0,length=8,status=0,type=b'linear',text=value)]
    check(name,table(specs),False)
specs=copy.deepcopy(multi);specs[1]['start']=4
check('multi_discontinuous',table(specs),False)
specs=copy.deepcopy(multi);specs[1]['length']=4
check('multi_incomplete',table(specs),False)
data=bytearray(base);data[312+40:312+48]=b'xxxxxxxx'
check('unterminated_parameters',bytes(data),False)
check('short_header',base[:311],False)
for kw,name in [(dict(event=72),'event_fingerprint'),(dict(uuid=b'changed'),'uuid_fingerprint')]:
    changed=check(name,table(**kw),True)
    assert changed!=fingerprint,name
data=table([dict(start=0,length=8,status=0,type=b'linear',text=b'8:1 129')])
assert check('extent_fingerprint',data,True)!=fingerprint

report=dict(status='PASS: offline host guard/UAPI review; phone not accessed',
            source_sha256=hashlib.sha256((ROOT/'recovery-guard.c').read_bytes()).hexdigest(),
            abi_static_assertions=ABI.count('SAME(')-1,cases=cases,device_operations=False,
            linux_uapi=dict(dm_ioctl='/usr/include/linux/dm-ioctl.h',statx='/usr/include/linux/stat.h'),
            limitations=['Synthetic DM replies exercise parser; no live DM ioctl/block device execution.',
                         'Host syscall constants/layout compiled; AArch64 execution untested.'])
(ROOT/'review.json').write_text(json.dumps(report,indent=2)+'\n')
print(ROOT/'review.json')
print(f'PASS {len(cases)} table cases and {report["abi_static_assertions"]} UAPI assertions')
