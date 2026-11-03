#!/usr/bin/env python3
"""Build the pinned 6.18 GKI and initial Onyx modules; archive and verify outputs."""

import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from verify_modules import verify

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--memory-mb", type=int, default=20000)
    parser.add_argument("--qdss-diagnostic", action="store_true",
                        help="temporarily apply reviewed opt-in AMBA/QDSS logging")
    args = parser.parse_args()
    if args.jobs < 1 or args.memory_mb < 1024:
        parser.error("jobs must be positive and memory-mb at least 1024")
    workspace = args.workspace.resolve()
    for script in ["prepare.py", "stage.py", "integrate_kernelsu.py"]:
        command = [sys.executable, str(HERE / script), str(workspace)]
        if script == "prepare.py":
            command.append("--verify-only")
        subprocess.run(command, check=True)

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    artifacts = workspace / "artifacts" / stamp
    artifacts.mkdir(parents=True)
    from verify_mi_thermal import verify as verify_mi_thermal
    thermal_validation = verify_mi_thermal(
        workspace / "onyx/drivers/thermal/xiaomi/mi_thermal_interface.c")
    (artifacts / "mi-thermal-host-validation.json").write_text(
        json.dumps(thermal_validation, indent=2) + "\n")
    from verify_device_services import verify as verify_device_services
    services_validation = verify_device_services(workspace / "onyx")
    (artifacts / "device-services-host-validation.json").write_text(
        json.dumps(services_validation, indent=2) + "\n")
    from verify_platform_providers import verify as verify_platform_providers
    platform_validation = verify_platform_providers(workspace / "onyx")
    (artifacts / "platform-providers-host-validation.json").write_text(
        json.dumps(platform_validation, indent=2) + "\n")
    command = ["tools/bazel", "build", "--keep_going", f"--jobs={args.jobs}",
               f"--local_resources=memory={args.memory_mb}",
               "--//build/kernel/kleaf:user_kmi_symbol_lists=//onyx:kmi-symbols",
               "//common:kernel_aarch64", "//onyx:platform_modules"]
    print(f"Building; log: {artifacts / 'build.log'}", flush=True)
    from qdss_diagnostic import applied
    from common_fixes import applied as common_fixes
    diagnostic = (applied(workspace / 'common', artifacts / 'common-diagnostic')
                  if args.qdss_diagnostic else nullcontext())
    with (artifacts / "build.log").open("w") as log:
        with diagnostic, common_fixes(workspace / 'common', artifacts / 'common-fixes'):
            subprocess.run(command, cwd=workspace, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    execution_root = Path(subprocess.check_output(
        ["tools/bazel", "info", "execution_root"], cwd=workspace, text=True).strip())
    query = subprocess.check_output([
        "tools/bazel", "cquery", "--output=files",
        "--//build/kernel/kleaf:user_kmi_symbol_lists=//onyx:kmi-symbols",
        "//common:kernel_aarch64 + //onyx:platform_modules"
    ], cwd=workspace, text=True)
    (artifacts / "bazel-outputs.txt").write_text(query)
    outputs = [execution_root / line for line in query.splitlines()]
    kernel_dir = next(p.parent for p in outputs if p.name == "Image")
    release = (kernel_dir / "include/config/kernel.release").read_text().strip()
    config = (kernel_dir / "kernel_aarch64_dot_config").read_text()
    if "\nCONFIG_KSU=y\n" not in config:
        raise ValueError("KernelSU Next must be built into the GKI")
    for option in ["KSU_DEBUG", "KSU_DISABLE_MANAGER", "KSU_DISABLE_POLICY"]:
        if f"\nCONFIG_{option}=y\n" in config:
            raise ValueError(f"unexpected KernelSU option: {option}")
    modules = [p for p in outputs if p.suffix == ".ko"]
    catalog = json.loads((HERE / "modules.json").read_text())
    if {p.name for p in modules} != {entry["output"] for entry in catalog}:
        raise ValueError("unexpected platform module set")
    symvers = [p for p in outputs if p.name.endswith("_Module.symvers")]
    verification = verify(modules, symvers, release)

    for path in outputs:
        category = "platform" if "onyx" in path.parts else "gki"
        target = artifacts / category / path.name
        if path.is_file():
            target.parent.mkdir(exist_ok=True)
            shutil.copyfile(path, target)
    for name in ["modules_staging_dir.tar.gz", "out-dir-kernel-headers.tar.gz"]:
        shutil.copyfile(kernel_dir / name, artifacts / "gki" / name)
    for entry in catalog:
        target = entry["target"]
        config = kernel_dir.parent.parent / "onyx" / (target + "_config") / "out_dir/.config"
        shutil.copyfile(config, artifacts / "platform" / (target + ".config"))
    (artifacts / "gki/kernel.release").write_text(release + "\n")
    subprocess.run([sys.executable, str(HERE / "build_devicetrees.py"), str(workspace),
                    "--output", str(artifacts / "devicetrees")], check=True)
    dt_report = json.loads((artifacts / "devicetrees/report.json").read_text())
    inputs = artifacts / "inputs"
    inputs.mkdir()
    for name in ["sources.json", "imports.json", "onyx-port.xml", "dependency-lock.xml",
                 "module-source-hashes.json", "common-source-hashes.json",
                 "BUILD.bazel", "Kconfig", "scm_defconfig", "platform_defconfig", "relay.fragment", "modules.json",
                 "build.py", "prepare.py", "stage.py", "integrate_kernelsu.py", "verify_modules.py", "verify_mi_thermal.py", "verify_device_services.py", "verify_platform_providers.py", "boot-parameters.txt",
                 "qdss_diagnostic.py", "common_fixes.py",
                 "build_devicetrees.py", "devicetrees.json", "kmi-symbols"]:
        shutil.copyfile(HERE / name, inputs / name)
    shutil.copytree(HERE / "patches", inputs / "patches")
    shutil.copytree(HERE / "common-patches", inputs / "common-patches")
    shutil.copytree(HERE / "kernelsu", inputs / "kernelsu")
    shutil.copyfile(HERE / "tests/mi_thermal_harness.c", inputs / "mi_thermal_harness.c")
    shutil.copytree(HERE / "tests", inputs / "tests",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    ksu_inputs = inputs / "kernelsu-next"
    ksu_inputs.mkdir()
    for name in ["source.json", "source.tar"]:
        shutil.copyfile(workspace / ".kernelsu-next" / name, ksu_inputs / name)
    shutil.copytree(HERE.parent / "validation/platform_providers", inputs / "platform-providers-tests",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "LOCAL*"))
    if (HERE / "validation").is_dir():
        shutil.copytree(HERE / "validation", inputs / "validation",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copyfile(workspace / "onyx/.staged-files.json", inputs / "staged-files.json")
    hashes = {}
    for path in sorted(artifacts.rglob("*")):
        if path.is_file():
            with path.open("rb") as stream:
                hashes[path.relative_to(artifacts).as_posix()] = hashlib.file_digest(
                    stream, "sha256").hexdigest()
    report = {
        "command": command,
        "qdss_diagnostic": args.qdss_diagnostic,
        "kernel_release": release,
        "kernelsu_next": {
            **json.loads((workspace / ".kernelsu-next/source.json").read_text()),
            "integration": "built-in CONFIG_KSU=y",
            "hardware_validated": False,
        },
        "module_verification": verification,
        "mi_thermal_host_validation": thermal_validation,
        "device_services_host_validation": services_validation,
        "platform_providers_host_validation": platform_validation,
        "devicetree_verification": {
            "overlay_engines_agree": dt_report["overlay_engines_agree"],
            "bootloader_ids_preserved": dt_report["bootloader_ids_preserved"],
            "dtbo_table_verified": dt_report["dtbo_table_verified"],
            "dtc_warnings": dt_report["dtc_warnings"],
            "report": "devicetrees/report.json",
        },
        "sha256": hashes,
        "limits": [
            "Build and imported-symbol CRC checks only; no device boot or hardware validation.",
            "Build archive only; assemble and verify matched boot and DLKM images before flashing.",
            "Gunyah secure-VM loading, proxy scheduling, memory assignment, and QRTR host transport are compiled together but have not been exercised with device firmware. Guest-power drivers are disabled, matching the Onyx HLOS baseline.",
            "The default Kleaf build uses the maybe-dirty suffix without stamping; input revisions are recorded separately.",
        ],
    }
    (artifacts / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Build and module CRC checks passed: {artifacts}")


if __name__ == "__main__":
    main()
