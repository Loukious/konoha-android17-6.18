#!/usr/bin/env python3
"""Exercise native CTI configuration, both MMIO layouts and rollback paths."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
PREFIX = "drivers/hwtracing/coresight/"


def function(source, name):
    match = re.search(r"(?m)^(?:static )?(?:inline )?[^\n;{}]*\b" + name + r"\([^;{}]*\)\n\{", source)
    if not match:
        raise ValueError("Missing function: " + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end].replace("__init ", "")


def verify(staged):
    sources = {name: (Path(staged) / (PREFIX + name)).read_text() for name in (
        "coresight-cti.h", "coresight-cti-core.c", "coresight-cti-sysfs.c", "coresight-cti-platform.c")}
    selected = {
        "coresight-cti.h": ["cti_channel_mask", "cti_reg_valid", "cti_active"],
        "coresight-cti-core.c": ["cti_write_all_hw_regs", "cti_enable_hw", "cti_cpuhp_enable_hw",
            "cti_disable_hw", "cti_write_single_reg", "cti_write_intack", "cti_set_default_config",
            "cti_channel_trig_op", "cti_channel_gate_op", "cti_channel_setop", "cti_disable_atclk",
            "cti_runtime_suspend", "cti_runtime_resume", "cti_suspend", "cti_resume", "cti_init",
            "cti_update_conn_xrefs", "cti_remove_conn_xrefs"],
        "coresight-cti-sysfs.c": ["inout_sel_store", "inen_store", "outen_store", "gate_store",
            "asicctl_store", "appset_store", "appclear_store", "apppulse_store"],
        "coresight-cti-platform.c": ["cti_plat_count_sig_elements", "cti_plat_read_trig_group",
            "cti_plat_process_filter_sigs"],
    }
    constants = "\n".join(line for line in sources["coresight-cti.h"].splitlines()
                          if line.startswith("#define ") and not line.startswith("#define _CORESIGHT"))
    functions = "\n".join(function(sources[name], item) for name, names in selected.items() for item in names)
    core = sources["coresight-cti-core.c"]
    assert "dev.release =" not in core
    assert core.index("drvdata->extended_cti = is_extended_cti(dev)") < core.index("cti_set_default_config(drvdata)")
    assert "raw_spin_lock(&drvdata->spinlock)" not in core
    assert "list_del_init(&drvdata->node)" in function(core, "cti_remove")
    assert "cti_cpu_drvdata[drvdata->ctidev.cpu] = NULL" in function(core, "cti_pm_release")
    assert "sysfs_remove_groups" in function(core, "cti_remove")
    harness = (HERE / "tests/coresight_cti_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-cti-test-") as directory:
        c = Path(directory) / "cti.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_CONSTANTS */", constants).replace("/* INSERT_FUNCTIONS */", functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    return {"status": "PASS", "cases": int(result.stdout.strip()), "sanitizers": "ASan+UBSan",
            "source_sha256": {PREFIX + name: hashlib.sha256((Path(staged) / (PREFIX + name)).read_bytes()).hexdigest()
                              for name in sources},
            "limits": ["MMIO, clocks and pinctrl are stubbed; physical cross-trigger delivery is not measured.",
                       "CPU pointer teardown and sysfs draining also require kernel/device validation."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
