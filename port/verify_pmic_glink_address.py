from pathlib import Path
import hashlib,json,re,subprocess,tempfile
R=Path(__file__).resolve().parent
driver=(R/'../modules/drivers/soc/qcom/pmic-glink-debug.c').read_text()
tested=driver[:driver.index('static int pmic_glink_debug_add_i2c_bus')]
harness=(R/'tests/pmic_glink_address_harness.c').read_text()
code=harness.replace('/* DRIVER */',re.sub(r'(?m)^#include[^\n]*\n','',tested))
with tempfile.TemporaryDirectory(prefix='onyx-pmic-glink-address-') as temp:
    d=Path(temp)
    def run(source):
        (d/'test.c').write_text(source)
        subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(d/'test.c'),'-o',str(d/'test')],check=True)
        return subprocess.run([str(d/'test')],capture_output=True,text=True,timeout=25)
    r=run(code);assert not r.returncode,r.stderr
    mutations={}
    variants={
        'restore-four-bit-i2c-vector-limit':code.replace('msgs[i].addr > 0x7f','msgs[i].addr > 0xf'),
        'restore-four-bit-shared-helper-limit':code.replace('sid > 0x7f || len > ((0x80 - sid)','sid > 0xf || len > ((0x10 - sid)'),
        'drop-spmi-namespace-limit':code.replace('if (sid > 0xf || len > ((0x10 - sid) * 0x10000UL - addr))','if (false)'),
        'accept-out-of-range-i2c-device':code.replace('msgs[i].addr > 0x7f','false'),
    }
    for name,source in variants.items():
        assert source!=code,name
        result=run(source);assert result.returncode,name;mutations[name]='REJECTED'
report=dict(status='PASS',cases=int(r.stdout),driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),tested_c_sha256=hashlib.sha256(tested.encode()).hexdigest(),
    harness_sha256=hashlib.sha256(harness.encode()).hexdigest(),sanitizers=['ASan','UBSan'],regression_mutations=mutations,
    limits=['Actual native read/write request handling, response fences, I2C vectors and SPMI/shared stream C; RPC replies simulated.',
            'Existing controller probe/removal code unchanged and outside this host harness. No live PMIC writes.'])
(R/'pmic-glink-address-host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
