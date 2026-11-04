from pathlib import Path
import json,hashlib,subprocess,tempfile
ROOT=Path(__file__).resolve().parent
source=(ROOT.parent/'modules/drivers/cpufreq/qcom-cpufreq-hw-debug.c').read_text()
core=source[source.index('enum debug_hw_regs_data'):source.index('static const struct of_device_id')]
harness=(ROOT/'tests/cpufreq_debug_harness.c').read_text().replace('/* DRIVER */',core)
with tempfile.TemporaryDirectory(prefix='onyx-cpufreq-debug-') as temp:
 p=Path(temp);c=p/'test.c';exe=p/'test'
 def run(s):
  c.write_text(s);subprocess.run(['cc','-std=gnu11','-g','-O1','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
  return subprocess.run([str(exe)],capture_output=True,text=True,timeout=45)
 result=run(harness);assert result.returncode==0,result.stderr
 small=run(harness.replace('#define PAGE_SIZE 4096','#define PAGE_SIZE 256'));assert small.returncode==0,small.stderr
 mutations={}
 for name,s in [('wrong-OF-node-release',harness.replace('of_node_put(args.np)','of_node_put(pdev->dev.of_node)')),('missing-panic-notifier-drain',harness.replace('atomic_notifier_chain_unregister(&panic_notifier_list, &regs->panic_nb);','/* skipped unregister */')),('unbounded-sysfs-buffer',harness.replace('#define PAGE_SIZE 4096','#define PAGE_SIZE 256').replace('PAGE_SIZE-offset,fmt,ap','PAGE_SIZE,fmt,ap'))]:
  broken=run(s);assert broken.returncode,name;mutations[name]='REJECTED'
report=dict(status='PASS',cases=int(result.stdout)+int(small.stdout),page_sizes_tested=[4096,256],source_sha256=hashlib.sha256(source.encode()).hexdigest(),harness_sha256=hashlib.sha256((ROOT/'tests/cpufreq_debug_harness.c').read_bytes()).hexdigest(),sanitizers=['ASan','UBSan'],regression_mutations=mutations,limits=['Actual driver C with simulated OF, MMIO, kobject, sysfs and notifier framework.','Panic callback runs offline only; no intentional phone panic.'])
(ROOT/'cpufreq-debug-host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
