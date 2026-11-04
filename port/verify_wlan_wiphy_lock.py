#!/usr/bin/env python3
"""Check actual HDD lock helpers for both call conventions and kernel branches."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
HEADER = "qcacld-3.0/core/hdd/inc/wlan_hdd_main.h"
HOSTAPD = "qcacld-3.0/core/hdd/src/wlan_hdd_hostapd.c"
REGULATORY = "qcacld-3.0/core/hdd/src/wlan_hdd_regulatory.c"


def verify(root):
    root = Path(root)
    source = (root / HEADER).read_text()
    start = source.index("#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)",
                         source.index("hdd_release_rtnl_lock"))
    helpers = source[start:source.index("#endif", start) + len("#endif")]
    # Audit the real caller conventions in addition to exercising the helpers.
    hostapd = (root / HOSTAPD).read_text()
    for name in ("hdd_chan_change_notify_update", "hdd_chan_change_started_notify"):
        start = hostapd.index("static void " + name + "(")
        end = hostapd.index("\n}\n", start)
        body = hostapd[start:end]
        assert body.count("hdd_wiphy_lock(NULL, dev->ieee80211_ptr);") == 1, name
        assert body.count("hdd_wiphy_unlock(NULL, dev->ieee80211_ptr);") == 1, name
    regulatory = (root / REGULATORY).read_text()
    assert "hdd_wiphy_lock(hdd_ctx->wiphy, NULL);" in regulatory
    assert "hdd_wiphy_unlock(hdd_ctx->wiphy, NULL);" in regulatory
    harness = (HERE / "tests/wlan_wiphy_lock.c").read_text()
    cases = 0
    with tempfile.TemporaryDirectory(prefix="onyx-wiphy-lock-") as directory:
        c = Path(directory) / "lock.c"
        c.write_text(harness.replace("/* INSERT_HELPERS */", helpers))
        for version in ((6, 6, 0), (6, 7, 0), (6, 18, 38)):
            exe = Path(directory) / ("lock-" + ".".join(map(str, version)))
            code = (version[0] << 16) | (version[1] << 8) | version[2]
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter",
                            "-O1", "-g", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                            f"-DLINUX_VERSION_CODE={code}", str(c), "-o", str(exe)],
                           check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True,
                                    check=True, timeout=10)
            cases += int(result.stdout.strip())
    return {"status": "PASS", "cases": cases, "sanitizers": "ASan+UBSan",
            "source_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                              for name in (HEADER, HOSTAPD, REGULATORY)},
            "limits": ["Lock primitives are host stubs; channel notification callers are audited, not executed.",
                       "Device hotspot startup and channel switching require runtime validation."]}


if __name__ == "__main__":
    print(json.dumps(verify(Path(sys.argv[1])), indent=2))
