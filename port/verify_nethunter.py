#!/usr/bin/env python3
"""Verify built NetHunter/CAN config and packaged native module coverage."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile


def verify_firmware(directory, wanted, sources):
    expected = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        checksum, name = line.split("  ", 1)
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or name in expected:
            raise ValueError("Unsafe or duplicated firmware inventory")
        expected[name] = checksum
    actual = {}
    for path in directory.rglob("*"):
        if path.is_symlink():
            raise ValueError("Unexpected firmware symlink")
        if path.is_file() and path.name != "SHA256SUMS":
            with path.open("rb") as stream:
                actual[path.relative_to(directory).as_posix()] = hashlib.file_digest(
                    stream, "sha256").hexdigest()
    if actual != expected:
        raise ValueError("Firmware/source/license hash inventory differs")
    manifest = json.loads((directory / "manifest.json").read_text())
    names = set()
    for blob in manifest["blobs"]:
        if (blob["path"] in names or
                actual.get("firmware/" + blob["path"]) != blob["sha256"]):
            raise ValueError("Firmware provenance differs from packaged bytes")
        if blob["license"] not in actual:
            raise ValueError("Firmware redistribution license is missing")
        names.add(blob["path"])
    if names != set(wanted) or len(wanted) != len(set(wanted)):
        raise ValueError("Firmware inventory differs from the selected wireless requirements")
    for source in sources:
        if actual.get(source["path"]) != source["sha256"]:
            raise ValueError("Required corresponding firmware source archive is missing or changed")
    return len(names)


def verify(artifact, port):
    requirements = json.loads((port / "nethunter-native-modules.json").read_text())
    release = (artifact / "gki/kernel.release").read_text().strip()
    if not release or "/" in release:
        raise ValueError("Invalid built kernel release")
    config = dict(line.split("=", 1) for line in
                  (artifact / "gki/kernel_aarch64_dot_config").read_text().splitlines()
                  if line.startswith("CONFIG_"))
    for name, value in requirements["configs"].items():
        if config.get("CONFIG_" + name) != value:
            raise ValueError("Missing native NetHunter/CAN config: " + name)
    if config.get("CONFIG_USB_CONFIGFS_RNDIS", "n") != "n":
        raise ValueError("GSI owns RNDIS gadget support on Onyx")
    modules = {}
    with tarfile.open(artifact / "gki/modules_staging_dir.tar.gz") as archive:
        for entry in archive:
            if not entry.name.endswith(".ko"):
                continue
            name = entry.name.removeprefix("./")
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or not entry.isfile():
                raise ValueError("Unsafe native module archive member")
            prefix = "lib/modules/" + release + "/kernel/"
            if not name.startswith(prefix):
                raise ValueError("Native module archive belongs to a different kernel release")
            relative = name.removeprefix(prefix)
            if relative in modules or entry.size == 0:
                raise ValueError("Duplicate or empty native module")
            modules[relative] = entry.size
    missing = set(requirements["modules"] + requirements["required_existing_modules"]) - modules.keys()
    if missing:
        raise ValueError("Native NetHunter/CAN modules were not packaged: " + ", ".join(sorted(missing)))
    firmware = verify_firmware(artifact / "firmware", requirements["wireless_firmware"],
                               requirements["firmware_source_archives"])
    return {"status": "PASS", "native_configs_verified": len(requirements["configs"]),
            "native_added_modules_verified": len(requirements["modules"]),
            "all_native_archive_modules": len(modules),
            "firmware_blobs_verified": firmware,
            "hardware_validated": False}
