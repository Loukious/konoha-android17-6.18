#!/usr/bin/env python3
"""Compile both native misc translation units and exercise voter lifetime."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
PREFIX = "drivers/power/supply/mca/mca_business/business_misc/"
SOURCES = [PREFIX + name for name in (
    "inc/mca_business_misc.h", "mca_business_votable.c", "mca_business_misc.c")]


def verify(staged):
    paths = [Path(staged) / name for name in SOURCES]
    source = "\n".join(re.sub(r"(?m)^#include[^\n]*\n", "", p.read_text()) for p in paths)
    harness = (HERE / "tests/business_misc_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="konoha-business-misc-") as directory:
        c = Path(directory) / "misc.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_DRIVER */", source))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, check=True, timeout=20)
    return {"status": "PASS", "cases": int(result.stdout.strip()), "sanitizers": "ASan+UBSan",
            "source_sha256": {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in zip(SOURCES, paths)},
            "limits": ["Voter and wakeup stubs verify rollback and teardown; kernel SRCU/provider lifetime is validated separately."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
