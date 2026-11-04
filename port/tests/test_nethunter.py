"""Regression checks for native archive identity and firmware completeness.

Archives use small synthetic module members because this gate checks packaging;
ELF, CRC and hardware checks belong to the build and device validation.
"""
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT))
from verify_nethunter import verify


class NativeFirmwareGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="konoha-native-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.artifact = Path(self.temporary.name)
        self.requirements = json.loads((PORT / "nethunter-native-modules.json").read_text())
        self.release = "6.18.38-android17-fixture"
        self.gki = self.artifact / "gki"
        self.gki.mkdir()
        (self.gki / "kernel.release").write_text(self.release + "\n")
        self.config = self.gki / "kernel_aarch64_dot_config"
        self.config.write_text("".join("CONFIG_" + k + "=" + v + "\n"
                                       for k, v in self.requirements["configs"].items()))
        self.modules = self.requirements["modules"] + self.requirements["required_existing_modules"]
        self.write_archive(self.release, self.modules)
        self.firmware = self.artifact / "firmware"
        shutil.copytree(PORT / "firmware", self.firmware)

    def write_archive(self, release, modules):
        with tarfile.open(self.gki / "modules_staging_dir.tar.gz", "w:gz") as archive:
            for name in modules:
                data = b"synthetic native module packaging fixture\n"
                info = tarfile.TarInfo("./lib/modules/" + release + "/kernel/" + name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))

    def refresh_firmware_inventory(self):
        entries = []
        for path in sorted(self.firmware.rglob("*")):
            if path.is_file() and path.name != "SHA256SUMS":
                entries.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " +
                               path.relative_to(self.firmware).as_posix() + "\n")
        (self.firmware / "SHA256SUMS").write_text("".join(entries))

    def test_complete_package_passes(self):
        self.assertEqual(verify(self.artifact, PORT)["status"], "PASS")

    def test_missing_native_config_rejected(self):
        lines = self.config.read_text().splitlines(keepends=True)
        self.config.write_text("".join(line for line in lines
                                       if not line.startswith("CONFIG_CAN_ISOTP=")))
        with self.assertRaisesRegex(ValueError, "Missing native.*CAN_ISOTP"):
            verify(self.artifact, PORT)

    def test_conflicting_rndis_gadget_rejected(self):
        self.config.write_text(self.config.read_text() + "CONFIG_USB_CONFIGFS_RNDIS=y\n")
        with self.assertRaisesRegex(ValueError, "GSI owns RNDIS"):
            verify(self.artifact, PORT)

    def test_missing_native_module_rejected(self):
        self.write_archive(self.release, [name for name in self.modules
                                         if name != "drivers/usb/serial/ch341.ko"])
        with self.assertRaisesRegex(ValueError, "modules were not packaged.*ch341"):
            verify(self.artifact, PORT)

    def test_wrong_archive_release_rejected(self):
        self.write_archive("6.6.0-wrong-release", self.modules)
        with self.assertRaisesRegex(ValueError, "different kernel release"):
            verify(self.artifact, PORT)

    def test_modified_firmware_rejected(self):
        blob = self.firmware / "firmware/rt2870.bin"
        data = blob.read_bytes()
        blob.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        with self.assertRaisesRegex(ValueError, "hash inventory differs"):
            verify(self.artifact, PORT)

    def test_missing_license_rejected_after_inventory_refresh(self):
        (self.firmware / "licenses/LICENCE.ralink-firmware.txt").unlink()
        self.refresh_firmware_inventory()
        with self.assertRaisesRegex(ValueError, "redistribution license is missing"):
            verify(self.artifact, PORT)

    def test_missing_source_rejected_after_inventory_refresh(self):
        next((self.firmware / "sources").glob("*.tar.gz")).unlink()
        self.refresh_firmware_inventory()
        with self.assertRaises(ValueError):
            verify(self.artifact, PORT)

    def test_missing_required_firmware_rejected_after_inventory_refresh(self):
        shutil.rmtree(self.firmware / "firmware")
        manifest_path = self.firmware / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["blobs"] = []
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        (self.firmware / "firmware.sha256").write_text("")
        self.refresh_firmware_inventory()
        with self.assertRaisesRegex(ValueError, "selected wireless requirements"):
            verify(self.artifact, PORT)


if __name__ == "__main__":
    unittest.main()
