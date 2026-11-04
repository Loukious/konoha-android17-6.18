#!/usr/bin/env python3
"""Exercise every ported qcom-battery attribute and class registration failure."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = "drivers/power/supply/mca/mca_qcom_sysfs/mca_qcom_sysfs.c"


def verify(staged):
    path = Path(staged) / SOURCE
    source = re.sub(r"(?m)^#include[^\n]*\n", "", path.read_text())
    harness = (HERE / "tests/qcom_sysfs_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-qcom-sysfs-") as directory:
        c = Path(directory) / "sysfs.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_DRIVER */", source))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-sign-compare", "-g", "-O1",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True)
    return {"status": "PASS", "cases": int(result.stdout.strip()),
            "sanitizers": "ASan+UBSan", "source_sha256": {
                SOURCE: hashlib.sha256(path.read_bytes()).hexdigest()},
            "limits": ["Full-driver provider stubs verify sysfs contracts and cleanup; no physical charger operations are performed."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
