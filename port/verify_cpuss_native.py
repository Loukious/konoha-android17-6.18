from pathlib import Path
import json,re,subprocess,tempfile,hashlib,sys
HERE=Path(__file__).resolve().parent
SOURCE='drivers/soc/qcom/qcom_cpuss_sleep_stats.c'
def verify(root):
 p=Path(root)/SOURCE;code=re.sub(r'(?m)^#include[^\n]*\n','',p.read_text())
 harness=(HERE/'tests/cpuss_native_harness.c').read_text().replace('/* INSERT_DRIVER */',code)
 with tempfile.TemporaryDirectory(prefix='onyx-cpuss-native-') as d:
  c=Path(d)/'driver.c';exe=c.with_suffix('');c.write_text(harness)
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-variable','-Wno-sign-compare','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
  if result.returncode:raise RuntimeError(result.stderr)
 return {'status':'PASS','cases':int(result.stdout.strip()),'sanitizers':'ASan+UBSan, recovery disabled','source_sha256':{SOURCE:hashlib.sha256(p.read_bytes()).hexdigest()},'limits':['Complete driver runs against bounded MMIO/SCM/debugfs/devres stubs. Hardware counter/reset behavior and actual debugfs proxy lifetime need kernel/device evidence.']}
if __name__=='__main__':print(json.dumps(verify(Path(sys.argv[1])),indent=2))
