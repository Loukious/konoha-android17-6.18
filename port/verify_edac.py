#!/usr/bin/env python3
"""Execute native EDAC setup, errors, IRQ and removal offline under sanitizers."""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parent
path=ROOT.parent/'modules/drivers/edac/qcom_edac.c';source=path.read_text()
header=ROOT.parent/'modules/include/linux/soc/qcom/llcc-qcom.h'
def function(name):
    m=re.search(r'(?m)^static (?:int|void|irqreturn_t)\s*\n?'+name+r'\([^;{}]*\)\n\{',source)
    if not m:raise ValueError(name)
    end=m.end();depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]
decls='\n'.join(re.search(r'struct '+n+r'\s*\{[^}]*\};',header.read_text()).group() for n in ['llcc_edac_reg_data','llcc_edac_reg_offset'])
decls+='\n'+source[source.index('#define EDAC_LLCC'):source.index('static void qcom_llcc_setup_registers')]
decls+=re.search(r'static const u32 qcom_llcc_setup_masks\[\] = \{[^}]*\};',source).group()
decls+=re.search(r'struct qcom_llcc_syn_regs \{[^}]*\};',source).group()
names=['qcom_llcc_setup_registers','qcom_llcc_restore_setup','qcom_llcc_core_setup','qcom_llcc_clear_error_status',
       'get_reg_offsets','dump_syn_reg_values','dump_syn_reg','llcc_ecc_irq_handler','llcc_ecc_check','qcom_llcc_edac_probe','qcom_llcc_edac_remove']
h=(ROOT/'tests/edac_harness.c').read_text().replace('/* DECLARATIONS */',decls).replace('/* FUNCTIONS */','\n'.join(function(n) for n in names))
with tempfile.TemporaryDirectory(prefix='onyx-edac-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(h)
    subprocess.run(['cc','-std=gnu11','-g','-O1','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
    result=subprocess.run([str(p/'test')],capture_output=True,text=True,timeout=40)
    if result.returncode:raise RuntimeError(result.stderr)
    sequential='''static irqreturn_t llcc_ecc_irq_handler(int irq, void *data)
{
    struct edac_device_ctl_info *ctl = data;
    struct qcom_llcc_edac *st = ctl->pvt_info;
    struct llcc_drv_data *drv = st->drv;
    irqreturn_t handled = IRQ_NONE;
    u32 i, status;
    mutex_lock(&st->lock);
    if (st->active) for (i = 0; i < drv->num_banks; i++) {
        if (!regmap_read(drv->regmaps[i], drv->edac_reg_offset->drp_interrupt_status, &status) && (status & 3)) {
            dump_syn_reg(ctl, status & 2 ? LLCC_DRAM_UE : LLCC_DRAM_CE, i);
            handled = IRQ_HANDLED;
        }
        if (!regmap_read(drv->regmaps[i], drv->edac_reg_offset->trp_interrupt_0_status, &status) && (status & 3)) {
            dump_syn_reg(ctl, status & 2 ? LLCC_TRAM_UE : LLCC_TRAM_CE, i);
            handled = IRQ_HANDLED;
        }
    }
    mutex_unlock(&st->lock);
    return handled;
}'''
    no_drain=function('qcom_llcc_edac_remove').replace('if (st->irq_requested)', 'if (false)')
    mutations={}
    for name,original,mutant in [('sequential-bank-clear',function('llcc_ecc_irq_handler'),sequential),
                                 ('free-before-IRQ-drain',function('qcom_llcc_edac_remove'),no_drain)]:
        (p/'test.c').write_text(h.replace(original,mutant))
        subprocess.run(['cc','-std=gnu11','-g','-O1','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
        broken=subprocess.run([str(p/'test')],capture_output=True,text=True,timeout=40)
        assert broken.returncode and 'Assertion' in broken.stderr,(name,broken.stderr)
        mutations[name]='REJECTED by actual-C assertions'
report={'status':'PASS','cases':int(result.stdout),'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'header_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),
        'harness_sha256':hashlib.sha256((ROOT/'tests/edac_harness.c').read_bytes()).hexdigest(),'functions':names,'sanitizers':['ASan','UBSan'],'regression_mutations':mutations,
        'limits':['Actual C with simulated regmap/EDAC/IRQ framework and pthread remove barrier.','No live ECC injection. Hardware register access and native polling/IRQ operation remain to be tested.']}
(ROOT/'edac-host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
