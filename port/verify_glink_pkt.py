#!/usr/bin/env python3
"""Exercise actual packet-provider functions with bounded MM/transport stubs."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
FUNCTIONS = [
    "glink_pkt_mapped_length", "glink_pkt_skb_data", "glink_pkt_kfree_skb",
    "glink_pkt_clear_queues", "glink_pkt_rpdev_no_copy_cb", "glink_pkt_rpdev_copy_cb",
    "glink_pkt_rpdev_cb", "glink_pkt_rpdev_sigs", "glink_pkt_drv_try_register",
    "glink_pkt_begin_unregister", "glink_pkt_finish_unregister",
    "glink_pkt_drv_try_unregister", "glink_pkt_idle_work", "glink_pkt_queue_idle",
    "glink_pkt_cancel_idle", "glink_pkt_open", "glink_pkt_release", "glink_pkt_read",
    "glink_pkt_write", "glink_pkt_tiocmset", "glink_pkt_mapping_vma", "glink_pkt_zap_mapping",
    "glink_pkt_revoke_pending", "glink_pkt_vma_close", "glink_pkt_vma_mremap",
    "glink_pkt_mmap", "glink_pkt_zerocopy_done", "glink_pkt_vaddr_to_page",
    "glink_pkt_mapping_extent", "glink_pkt_zerocopy_receive",
    "glink_pkt_zerocopy_rollback", "glink_pkt_ioctl",
]


def function(source, name):
    matches = re.finditer(r"(?m)^(?:static )?[A-Za-z_][^;\n]+\b" + name + r"\(", source)
    match = next((m for m in matches if ";" not in source[m.end():source.index("{", m.end())]), None)
    if match is None:
        raise ValueError(name)
    brace = source.index("{", match.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def verify_signals(root):
    path = Path(root) / "drivers/rpmsg/qcom_glink_native.c"
    source = path.read_text()
    code = "\n\n".join(function(source, name) for name in
                       ["qcom_glink_handle_signals", "qcom_glink_register_signals_cb"])
    harness = (HERE / "tests/glink_signals_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="onyx-glink-signals-tests-") as directory:
        c = Path(directory) / "signals.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_SIGNALS */", code))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                        "-fno-pie", "-no-pie", "-pthread", str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=20)
    return {"status": "PASS", "result": result.stdout.strip(),
            "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def verify(root=HERE):
    root = Path(root)
    path = root / "drivers/rpmsg/glink_pkt.c"
    source = path.read_text()
    declarations = source[source.index("#define GLINK_PKT_IOCTL_MAGIC"):source.index("#define MODULE_NAME")]
    declarations += source[source.index("struct glink_pkt_device {"):source.index("static const struct vm_operations_struct glink_pkt_vm_ops;")]
    declarations += "\n#define cdev_to_gpdev(p) container_of(p, struct glink_pkt_device, cdev)\n"
    declarations += "static const struct vm_operations_struct glink_pkt_vm_ops;\n"
    declarations += "static void glink_pkt_revoke_pending(struct glink_pkt_device *gpdev);\n"
    functions = "\n\n".join(function(source, name) for name in FUNCTIONS)
    position = functions.index("static int glink_pkt_mmap(")
    functions = functions[:position] + "static const struct vm_operations_struct glink_pkt_vm_ops = {.close=glink_pkt_vma_close,.mremap=glink_pkt_vma_mremap};\n" + functions[position:]
    harness = (HERE / "tests/glink_packet_harness.c").read_text()
    with tempfile.TemporaryDirectory(prefix="onyx-glink-packet-tests-") as directory:
        c = Path(directory) / "packet.c"
        exe = c.with_suffix("")
        c.write_text(harness.replace("/* INSERT_DRIVER */", declarations + functions))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                        "-Wno-sign-compare", "-fno-strict-aliasing", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                        "-fno-pie", "-no-pie", str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=40)
        if result.returncode:
            raise RuntimeError(result.stderr)
    return {"status": "PASS", "assertions": int(result.stdout.strip()),
            "sanitizers": "ASan+UBSan", "functions": FUNCTIONS,
            "source_sha256": {"drivers/rpmsg/glink_pkt.c": hashlib.sha256(path.read_bytes()).hexdigest(),
                              "drivers/rpmsg/qcom_glink_native.c": hashlib.sha256((root / "drivers/rpmsg/qcom_glink_native.c").read_bytes()).hexdigest()},
            "signals": verify_signals(root),
            "limits": ["MM/PTE, firmware ACK, waits and workqueue scheduling are host stubs.",
                       "No runtime playback, physical DSP mapping, or platform probe/unload validation is claimed."]}


if __name__ == "__main__":
    print(json.dumps(verify(sys.argv[1]), indent=2))
