from pathlib import Path
import hashlib,json,re,subprocess,tempfile
R=Path(__file__).resolve().parent
driver=(R/'../modules/drivers/mfd/qcom-i2c-pmic.c').read_text()
harness=(R/'tests/i2c_pmic_harness.c').read_text()
code=harness.replace('/* DRIVER */',re.sub(r'(?m)^#include[^\n]*\n','',driver))
with tempfile.TemporaryDirectory(prefix='onyx-i2c-pmic-') as temp:
    d=Path(temp)
    def run(source):
        (d/'test.c').write_text(source)
        subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(d/'test.c'),'-o',str(d/'test')],check=True)
        return subprocess.run([str(d/'test')],capture_output=True,text=True,timeout=30)
    r=run(code);assert not r.returncode,r.stderr
    mutations={}
    variants={
        'index-unused-summary-tail-bits':code.replace('if (first_periph + i >= chip->num_periphs)','if (false)'),
        'accept-truncated-peripheral-address':code.replace('if (temp > 0xff)','if (false)'),
        'ignore-regmap-error-pointer':code.replace('if (IS_ERR(chip->regmap))','if (!chip->regmap)'),
        'lose-partial-enable-sync':code.replace('periph->synced[IRQ_EN_SET] &= ~en_clr;',';'),
        'discard-earlier-suspend-mask-error':code.replace('if (rc && !first_error)\n\t\t\tfirst_error = rc;','first_error = rc;'),
        'remove-domain-before-parent-irq':code.replace('devm_free_irq(chip->dev, chip->summary_irq, chip);','irq_domain_remove(chip->domain); devm_free_irq(chip->dev, chip->summary_irq, chip);'),
    }
    for name,source in variants.items():
        assert source!=code,name
        result=run(source);assert result.returncode,name;mutations[name]='REJECTED'
report=dict(status='PASS',cases=int(r.stdout),driver_sha256=hashlib.sha256(driver.encode()).hexdigest(),harness_sha256=hashlib.sha256(harness.encode()).hexdigest(),
    sanitizers=['ASan','UBSan'],regression_mutations=mutations,
    limits=['Complete actual native driver C; I2C/regmap, IRQ domain, devres, MFD children and system PM simulated.',
            'IRQ toggle self-test executes only against host mocks; Onyx has no interrupt-controller or toggle property on the two debug clients.'])
(R/'i2c-pmic-host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
