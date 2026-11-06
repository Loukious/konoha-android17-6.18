from pathlib import Path
import subprocess,hashlib,json,os,time,fcntl,struct
root=Path(__file__).parent
exe=root/'guard-test'
checks=[]
def run(args,ok=None,timeout=1):
 p=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True,timeout=timeout)
 if ok is not None:assert (p.returncode==0)==ok,(args,p.returncode,p.stdout)
 return p
def record(name,ok):checks.append({'case':name,'status':'PASS'});assert ok,name
def fresh(name,state=b'',snap=True):
 p=root/name;p.mkdir()
 if state is not None:(p/'state').write_bytes(state)
 if snap:(p/'snapshots').mkdir()
 return p
proto={'empty':(b'',True),'legacy_none':(b'none',True),'legacy_none_newline':(b'none\n',False),'legacy_active':(b'merging',False),'field1_none':(b'\x08\x00',True),'field1_active':(b'\x08\x01',False),'field18_default':(b'\x90\x01\x00',True),'field8_empty':(b'\x42\x00',True),'field8_stale':(b'\x42\x01x',False),'duplicate_none':(b'\x08\x00\x08\x00',False),'forbidden_field11':(b'\x58\x00',False),'unknown_field19':(b'\x98\x01\x00',False),'field0':(b'\x00',False),'field_wrong_wire':(b'\x0a\x00',False),'noncanonical_key':(b'\x88\x00\x00',False),'noncanonical_value':(b'\x08\x80\x00',False),'truncated_key':(b'\x80',False),'truncated_value':(b'\x08',False),'overflow_key':(b'\xff'*10,False),'overflow_value':(b'\x08'+b'\xff'*9+b'\x02',False),'huge_file':(b'x'*4097,False)}
for name,(data,ok) in proto.items():
 path=fresh('state_'+name,data)
 p=run(['test-state',path],ok)
 record('state_'+name,True)
# Unsupported state and snapshot inode checks use actual host syscalls.
path=fresh('missing_state',None);run(['test-state',path],False);record('missing_state',True)
path=fresh('missing_snapshots',b'',False);run(['test-state',path],False);record('missing_snapshots',True)
path=fresh('state_symlink',None);(path/'state').symlink_to(root/'state_empty/state');run(['test-state',path],False);record('state_symlink',True)
path=fresh('state_directory',None);(path/'state').mkdir();run(['test-state',path],False);record('state_directory',True)
path=fresh('state_fifo',None);os.mkfifo(path/'state');run(['test-state',path],False);record('state_fifo_no_hang',True)
path=fresh('snapshot_symlink',b'',False);(path/'snapshots').symlink_to(root/'state_empty/snapshots');run(['test-state',path],False);record('snapshot_symlink',True)
for name,kind in [('record','file'),('hidden','.hidden'),('subdir','dir'),('link','link')]:
 path=fresh('snapshot_'+name)
 target=path/'snapshots'/('.hidden' if kind=='.hidden' else name)
 if kind=='dir':target.mkdir()
 elif kind=='link':target.symlink_to(root/'state_empty/state')
 else:target.write_text('stale')
 run(['test-state',path],False);record('snapshot_'+name,True)
path=root/'metadata_symlink';path.symlink_to(root/'state_empty');run(['test-state',path],False);record('metadata_symlink',True)
run(['block',root/'state_empty/state'],False);record('block_regular_file_rejected',True)
run(['block',root/'state_fifo/state'],False);record('block_fifo_no_hang',True)
# Native supervising parent holds flock throughout actual fork/exec/wait4.
wrapper=root/'busybox-host-wrapper';wrapper.write_text('#!/usr/bin/python3\nimport os,sys\nassert sys.argv[1]=="sh"\nos.execv("/bin/sh",["/bin/sh",sys.argv[2]])\n');wrapper.chmod(0o755)
for code in [0,7,127]:
 path=fresh('run_exit_'+str(code));script=root/('exit_'+str(code)+'.sh');script.write_text('exit '+str(code)+'\n')
 p=run(['test-run',path,wrapper,script]);record('supervisor_exit_'+str(code),p.returncode==code)
path=fresh('run_signal');script=root/'signal.sh';script.write_text('kill -TERM $$\n')
p=run(['test-run',path,wrapper,script]);record('supervisor_signal',p.returncode!=0 and 'interrupted' in p.stdout)
path=fresh('run_bad_exec');p=run(['test-run',path,root/'absent-exec',root/'exit_0.sh']);record('supervisor_exec_failure',p.returncode==127)
path=fresh('run_lock');script=root/'hold.sh';marker=root/'started';script.write_text('echo ready > '+str(marker)+'\nsleep 2\nexit 0\n')
p=subprocess.Popen([str(exe),'test-run',str(path),str(wrapper),str(script)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
 deadline=time.monotonic()+1
 while not marker.exists() and time.monotonic()<deadline:time.sleep(.01)
 assert marker.exists(),'supervised child did not start'
 probe=run(['test-state',path],False);record('supervisor_metadata_lock_held','lock busy' in probe.stdout)
 assert p.wait(timeout=4)==0
 run(['test-state',path],True);record('supervisor_metadata_lock_released',True)
finally:
 if p.poll() is None:p.kill();p.wait()
path=fresh('run_lock_busy');fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY)
try:
 fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
 p=run(['test-run',path,wrapper,root/'exit_0.sh'],False);record('supervisor_lock_busy_refuses','lock busy' in p.stdout)
finally:os.close(fd)
path=fresh('run_dirty',b'\x08\x01');p=run(['test-run',path,wrapper,root/'exit_0.sh'],False);record('supervisor_dirty_state_refuses',True)
# The production target is static AArch64 ELF without runtime test paths.
data=(root/'guard-aarch64').read_bytes();assert data[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',data,18)[0]==183
phoff=struct.unpack_from('<Q',data,32)[0];phsize,phnum=struct.unpack_from('<HH',data,54)
assert all(struct.unpack_from('<I',data,phoff+i*phsize)[0]!=3 for i in range(phnum))
assert b'test-state' not in data and b'test-table' not in data and b'test-run' not in data
record('production_aarch64_static_no_test_entry',True)
summary={'status':'PASS','guard_source_sha256':hashlib.sha256((root/'recovery-guard.c').read_bytes()).hexdigest(),'production_aarch64_sha256':hashlib.sha256(data).hexdigest(),'scenario_count':len(checks),'checks':checks,'runtime':'native host x86_64 actual syscall tests; AArch64 static ELF inspected, device untested','production_changes':False,'device_operations':False}
(root/'review.json').write_text(json.dumps(summary,indent=2)+'\n')
print('PASS',len(checks),'scenarios',root/'review.json')
