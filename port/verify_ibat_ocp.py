#!/usr/bin/env python3
"""Run the complete adapted IBAT OCP driver against failing real-provider contracts."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
PREFIX = "drivers/power/supply/mca/mca_hardware_monitor/"
SOURCES = [PREFIX + n for n in ("inc/mca_ibat_ocp_monitor.h", "mca_ibat_ocp_monitor.c")]


def verify(staged):
    paths = [Path(staged) / n for n in SOURCES]
    source = "\n".join(re.sub(r"(?m)^#include[^\n]*\n", "", p.read_text()) for p in paths)
    harness = (HERE / "tests/ibat_ocp_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-ibat-ocp-") as directory:
        c = Path(directory) / "ibat.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_DRIVER */", source))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                        "-Wno-sign-compare", "-Wno-misleading-indentation", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True, timeout=20)
    return {"status": "PASS", "cases": int(result.stdout.strip()), "sanitizers": "ASan+UBSan",
            "source_sha256": {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in zip(SOURCES, paths)},
            "limits": ["Full-driver provider, DT, sysfs and workqueue stubs verify control flow; no physical overcurrent is injected."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
