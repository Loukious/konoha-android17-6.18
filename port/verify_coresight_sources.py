#!/usr/bin/env python3
"""Exercise dummy/remote ETM sources with actual CSR multi-ID routing."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from verify_coresight_cti import function

HERE = Path(__file__).resolve().parent
PREFIX = "drivers/hwtracing/coresight/"


def verify(staged):
    paths = {name: Path(staged) / (PREFIX + name) for name in (
        "coresight-dummy.c", "coresight-remote-etm.c", "coresight-csr.c", "coresight-csr.h")}
    sources = {name: path.read_text() for name, path in paths.items()}
    declarations = "\n".join(sources[name][sources[name].index("struct " + struct_name + " {"):
        sources[name].index("\n};", sources[name].index("struct " + struct_name + " {")) + 3]
        for name, struct_name in [("coresight-dummy.c", "dummy_drvdata"),
                                  ("coresight-remote-etm.c", "remote_etm_drvdata"),
                                  ("coresight-csr.c", "csr_drvdata")])
    declarations = "\n".join(line for line in sources["coresight-csr.c"].splitlines() if line.startswith("#define CSR_")) + "\n" + declarations
    selected = {
        "coresight-csr.c": ["csr_bank", "csr_route", "qcom_csr_route_ids"],
        "coresight-dummy.c": ["dummy_source_get_qmi_device", "qmi_assign_dummy_source_atid", "dummy_source_enable",
            "dummy_source_disable", "dummy_trace_id", "dummy_sink_enable", "dummy_sink_disable", "dummy_probe", "dummy_remove"],
        "coresight-remote-etm.c": ["remote_etm_get_qmi_device", "qmi_assign_remote_etm_atid", "qmi_enable_remote_etm",
            "qmi_disable_remote_etm", "remote_etm_release_ids", "remote_etm_enable", "remote_etm_disable",
            "remote_etm_trace_id", "remote_etm_get_traceid", "remote_etm_probe", "remote_etm_remove",
            "remote_etm_suspend", "remote_etm_resume"],
    }
    functions = "\n".join(function(sources[name], item) for name, names in selected.items() for item in names)
    harness = (HERE / "tests/coresight_sources_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-source-test-") as directory:
        c = Path(directory) / "sources.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_DECLARATIONS */", declarations).replace("/* INSERT_FUNCTIONS */", functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            result.check_returncode()
    return {"status": "PASS", "cases": int(result.stdout.strip()), "sanitizers": "ASan+UBSan",
            "source_sha256": {PREFIX + name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()},
            "limits": ["QMI and MMIO are stubbed; hardware trace delivery remains untested.",
                       "Unconfirmed firmware stops retain trace IDs to prevent unsafe reuse."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
