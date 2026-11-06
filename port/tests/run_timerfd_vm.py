#!/usr/bin/env python3
"""Compare timerfd resume/freezer behavior in two disposable x86 QEMU kernels.

The test include freezes kernel workqueues; it is only for these VM builds.
Clock changes and timerfd syscalls run inside the VM, never on the host.
Requires the kernel build tools, a static C runtime, and qemu-system-x86_64.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess

HERE = Path(__file__).resolve().parent
ENABLE = """
64BIT SMP SUSPEND SUSPEND_FREEZER KUNIT KUNIT_DEFAULT_ENABLED
KUNIT_AUTORUN_ENABLED DEBUG_KERNEL FTRACE ENABLE_DEFAULT_TRACERS TIMERFD EPOLL
POSIX_TIMERS HIGH_RES_TIMERS PRINTK TTY SERIAL_8250 SERIAL_8250_CONSOLE
BINFMT_ELF BLK_DEV_INITRD RD_GZIP PROC_FS SYSFS DEVTMPFS X86_IOPL_IOPERM
KERNEL_GZIP MULTIUSER ACPI X86_LOCAL_APIC X86_IO_APIC NO_HZ_IDLE PREEMPT_NONE
COREDUMP RTC_CLASS RTC_DRV_CMOS
""".split()
DISABLE = """
KERNEL_XZ KUNIT_ALL_TESTS KUNIT_TEST KUNIT_EXAMPLE_TEST WERROR RUST DEBUG_INFO
SLUB_TINY FUNCTION_TRACER MODULES RANDOMIZE_BASE RELOCATABLE
""".split()
API_CASES = {
    "clock-1-expiry", "clock-7-expiry", "clock-0-expiry", "clock-9-expiry",
    "clock-8-expiry", "disarmed-cancel-on-set-setup",
    "disarmed-clock-change-cancels", "second-clock-change-cancels",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_kernel(source, output, jobs):
    output.mkdir(parents=True, exist_ok=True)
    make = ["make", "-s", "-C", str(source), "ARCH=x86_64", "O=" + str(output)]
    with (output / "build.log").open("w") as log:
        def run(command):
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           check=True)

        run(make + ["tinyconfig"])
        config = output / ".config"
        command = [str(source / "scripts/config"), "--file", str(config)]
        for name in ENABLE:
            command += ["--enable", name]
        for name in DISABLE:
            command += ["--disable", name]
        for name, value in {"NR_CPUS": 2, "SERIAL_8250_NR_UARTS": 1,
                            "SERIAL_8250_RUNTIME_UARTS": 1}.items():
            command += ["--set-val", name, str(value)]
        run(command)
        run(make + ["olddefconfig"])
        resolved = config.read_text()
        for name in ENABLE + ["FREEZER", "TRACEPOINTS", "PM_SLEEP"]:
            if f"CONFIG_{name}=y\n" not in resolved:
                raise ValueError(f"Missing VM config: {name}")
        run(make + [f"-j{jobs}",
                    "CFLAGS_timerfd.o=-include " + str(HERE / "timerfd_resume_vm_test.h"),
                    "bzImage"])


def build_initramfs(output):
    init = output / "init"
    subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(HERE / "timerfd_vm_init.c"), "-o", str(init)], check=True)
    archive = bytearray()

    def entry(name, mode, content=b"", major=0, minor=0):
        encoded = name.encode() + b"\0"
        fields = [len(archive) + 1, mode, 0, 0, 1, 0, len(content), 0, 0,
                  major, minor, len(encoded), 0]
        archive.extend(b"070701" + "".join(f"{v:08x}" for v in fields).encode()
                       + encoded)
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(content)
        archive.extend(b"\0" * (-len(archive) % 4))

    for name in ["dev", "proc", "sys"]:
        entry(name, stat.S_IFDIR | 0o755)
    entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1)
    entry("init", stat.S_IFREG | 0o755, init.read_bytes())
    entry("TRAILER!!!", 0)
    path = output / "initramfs.cpio"
    path.write_bytes(archive)
    return path


def run_vm(args, name, kernel, initramfs):
    command = [args.qemu, "-machine", "q35,accel=tcg", "-cpu", "max",
               "-smp", "2", "-m", "512", "-nodefaults", "-display", "none",
               "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
               "-kernel", str(kernel), "-initrd", str(initramfs), "-append",
               "console=ttyS0 rdinit=/init panic=-1 "
               "kunit.filter_glob=timerfd-resume-freezer*"]
    if args.qemu_data:
        command += ["-L", str(args.qemu_data.resolve())]
    if args.qemu_bios:
        command += ["-bios", str(args.qemu_bios.resolve())]
    path = args.output / (name + "-run.log")
    with path.open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                timeout=120, check=False)
    output = path.read_text()
    # QEMU's isa-debug-exit turns the guest success value 0x10 into status 33.
    if result.returncode != 33 or "TIMERFD_API_END failures=0" not in output:
        raise ValueError(f"Guest/API failure: {path}")
    if set(re.findall(r"TIMERFD_API PASS ([^\s]+)", output)) != API_CASES:
        raise ValueError(f"Missing timerfd API results: {path}")
    match = re.search(r"# timerfd-resume-freezer: pass:(\d+) fail:(\d+) "
                      r"skip:(\d+) total:(\d+)", output)
    expected = (1, 2, 0, 3) if name == "baseline" else (3, 0, 0, 3)
    if not match or tuple(map(int, match.groups())) != expected:
        raise ValueError(f"Unexpected freezer results: {path}")
    return {"qemu_exit": result.returncode, "freezer_pass": expected[0],
            "freezer_fail": expected[1], "api_pass": len(API_CASES),
            "image_sha256": sha(kernel), "log_sha256": sha(path),
            "command": command}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="unmodified common source")
    parser.add_argument("candidate", type=Path, help="patched common source")
    parser.add_argument("output", type=Path, help="dedicated VM build directory")
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-data", type=Path)
    parser.add_argument("--qemu-bios", type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    if not shutil.which(args.qemu):
        parser.error("qemu-system-x86_64 is required")
    args.output = args.output.resolve()
    # Requiring a new directory keeps previous evidence and source trees safe.
    args.output.mkdir(parents=True, exist_ok=False)
    initramfs = build_initramfs(args.output)
    report = {"status": "PASS", "test_sha256": {
        name: sha(HERE / name) for name in
        ["timerfd_resume_vm_test.h", "timerfd_vm_init.c", "run_timerfd_vm.py"]}}
    for name in ["baseline", "candidate"]:
        source = getattr(args, name).resolve()
        output = args.output / name
        print(f"Building {name}; log: {output / 'build.log'}", flush=True)
        source_sha = sha(source / "fs/timerfd.c")
        build_kernel(source, output, args.jobs)
        result = run_vm(args, name, output / "arch/x86/boot/bzImage", initramfs)
        if sha(source / "fs/timerfd.c") != source_sha:
            raise ValueError(f"{name} source changed during verification")
        report[name] = dict(result, source_sha256=source_sha,
                            config_sha256=sha(output / ".config"))
    report["limits"] = ["VM workqueue freezer and timerfd API tests; no phone "
                        "sleep-residency, firmware or WLAN validation."]
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
