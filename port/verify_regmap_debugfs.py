from pathlib import Path
import hashlib,json,re,subprocess,tempfile
R=Path(__file__).resolve().parent
driver=(R/'../modules/drivers/base/regmap/qti-regmap-debugfs.c').read_text()
harness=(R/'tests/regmap_debugfs_harness.c').read_text()
code=harness.replace('/* DRIVER */',re.sub(r'(?m)^#include[^\n]*\n','',driver))
with tempfile.TemporaryDirectory(prefix='onyx-regmap-debugfs-') as temp:
    d=Path(temp)
    def run(source):
        (d/'test.c').write_text(source)
        subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(d/'test.c'),'-o',str(d/'test')],check=True)
        return subprocess.run([str(d/'test')],capture_output=True,text=True,timeout=40)
    r=run(code);assert not r.returncode,r.stderr
    mutations={}
    variants={
        'restore-unprotected-data-file':code.replace('debugfs_create_file("data",','debugfs_create_file_unsafe("data",'),
        'match-wrong-managed-release-callback':code.replace('devres_release(dev, devm_regmap_qti_debugfs_release,','devres_release(dev, _devm_regmap_qti_debugfs_release,'),
        'accept-count-overflow':code.replace('if (value > (u64)debug_map->regmap->max_register / debug_map->regmap->reg_stride + 1)','if (false)'),
        'accept-address-overflow':code.replace('if (value > debug_map->regmap->max_register || value % debug_map->regmap->reg_stride)','if (false)'),
        'skip-data-extent-bound':code.replace('entries - 1 > (map->max_register - debug_map->dump_address) / map->reg_stride','false'),
    }
    for name,source in variants.items():
        assert source!=code,name
        result=run(source);assert result.returncode,name;mutations[name]='REJECTED'
report=dict(status='PASS',cases=int(r.stdout),driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),harness_sha256=hashlib.sha256(harness.encode()).hexdigest(),
    sanitizers=['ASan','UBSan'],regression_mutations=mutations,
    limits=['Complete actual native diagnostic/register-cache/read/public registration C; Linux debugfs proxy, devres and regmap hardware are simulated.',
            'Hardware writes remain gated by the existing kernel configuration and are not exercised live.'])
(R/'regmap-debugfs-host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
