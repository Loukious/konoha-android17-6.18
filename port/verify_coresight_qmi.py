#!/usr/bin/env python3
"""Exercise actual CoreSight QMI transactions, endpoint state and probe rollback."""
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
    paths = [Path(staged) / (PREFIX + name) for name in ("coresight-qmi.c", "coresight-qmi.h")]
    source, header = [p.read_text() for p in paths]
    selected = ["service_coresight_qmi_new_server", "service_coresight_qmi_del_server",
                "coresight_qmi_request", "coresight_qmi_remote_etm_enable", "coresight_qmi_remote_etm_disable",
                "coresight_qmi_etr_assign", "coresight_qmi_assign_atid", "qmi_helper_enable", "qmi_helper_disable",
                "coresight_qmi_probe", "coresight_qmi_remove"]
    functions = "\n".join(function(source, name) for name in selected)
    header = "\n".join(line for line in header.splitlines() if not line.startswith("#include "))
    harness = (HERE / "tests/coresight_qmi_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-qmi-test-") as directory:
        c = Path(directory) / "qmi.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_HEADER */", header).replace("/* INSERT_FUNCTIONS */", functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    return {"status": "PASS", "cases": int(result.stdout.strip()), "sanitizers": "ASan+UBSan",
            "source_sha256": {PREFIX + p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
            "limits": ["QMI transport is stubbed; firmware behavior and remote trace delivery need device validation."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
