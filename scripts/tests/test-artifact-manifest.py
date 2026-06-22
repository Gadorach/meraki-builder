#!/usr/bin/env python3
"""Exercise complete-image release manifest finalization without hardware."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts/write-artifact-manifest.py"
TOTAL_BYTES = 0x1000000
LOADER_BYTES = 0x40000
KERNEL_OFFSET = 0x40000
ROOTFS_OFFSET = 0x300000
GEOMETRY = {
    "bytes": TOTAL_BYTES,
    "erase_bytes": 0x10000,
    "page_bytes": 256,
    "address_bytes": 3,
}
JEDEC = ["c22018", "ef4018", "012018", "20ba18", "c84018"]
TARGETS = {
    "luton26": {
        "id": 1,
        "spi": 0x70000064,
        "models": ["MS22", "MS22P", "MS220-8", "MS220-8P", "MS220-24", "MS220-24P"],
    },
    "jaguar1": {
        "id": 2,
        "spi": 0x70000068,
        "models": [
            "MS320-24", "MS320-24P", "MS220-48", "MS220-48P", "MS220-48LP",
            "MS220-48FP", "MS320-48", "MS320-48P", "MS320-48LP", "MS320-48FP",
            "MS42", "MS42P",
        ],
    },
}


class ArtifactManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="postmerkos-artifact-manifest-")
        self.root = Path(self.temp.name)
        self.recovery = self.root / "recovery"
        self.recovery.mkdir()

        image = bytearray(b"\xff" * TOTAL_BYTES)
        markers = (b"PMOSRAM READY 2", b"PMOSBOOT MENU-PROBE", b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY")
        cursor = 0x100
        for marker in markers:
            image[cursor:cursor + len(marker)] = marker
            cursor += len(marker) + 16
        kernel = b"fixture-kernel"
        kernel += b"\0" * ((-len(kernel)) % 32)
        header = struct.Struct("<8I")
        words = [0x4D495053, 0x81000000, len(kernel), 0x81000000, 0, 0, 0, 0]
        words[4] = zlib.crc32(header.pack(*words) + kernel) & 0xFFFFFFFF
        image[KERNEL_OFFSET:KERNEL_OFFSET + header.size + len(kernel)] = header.pack(*words) + kernel
        image[ROOTFS_OFFSET:ROOTFS_OFFSET + 4] = b"hsqs"
        self.image = self.root / "firmware.bin"
        self.image.write_bytes(image)
        self.rootfs = self.root / "rootfs.squashfs"
        self.rootfs.write_bytes(b"hsqs" + b"fixture-rootfs")

        all_models = [model for target in TARGETS.values() for model in target["models"]]
        self.source = self.root / "release-source.json"
        self.source.write_text(json.dumps({
            "version": "fixture",
            "models": {model: "untested" for model in all_models},
        }, indent=2) + "\n")
        self.output = self.root / "release-final.json"

        embedded = {}
        for family, target in TARGETS.items():
            marker = (
                f"PMOSRECOVERY3;SOC={family};FAMILY={target['id']};"
                f"SPI={target['spi']:08x};PROTO=3;PREFLIGHT=4;BAUDTEST=1;FRAME_MAX=4096;"
                "WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;CONFIRM_RETRY=1;"
                "AUTO_CONFIRM=1;AUTO_REBOOT=1;END"
            ).encode("ascii")
            payload = self.recovery / f"recovery-{family}.bin"
            payload.write_bytes(b"payload-prefix\0" + marker + b"\0payload-suffix")
            digest = hashlib.sha256(payload.read_bytes()).hexdigest()
            embedded[family] = {
                "path": str(payload), "size": payload.stat().st_size, "sha256": digest,
                "load_address": 0x81000000, "entry_address": 0x81000000,
                "entry_contract": "flat-binary-byte-zero-v1",
                "manifest_lookup_contract": "direct-object-members-v1",
                "hardware_preflight_contract": "spi-nor-scratch-rw-restore-loader-crc-v4",
                "spi_master_enable_contract": "preserve-general-ctrl-enable-spi-v1",
                "adaptive_transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
            }
            descriptor = {
                "format": "postmerkos.uart-recovery-payload.v3",
                "protocol_version": 3,
                "soc_family": family,
                "soc_family_id": target["id"],
                "spi_software_mode_address": target["spi"],
                "accepted_models": target["models"],
                "accepted_flash_bytes": TOTAL_BYTES,
                "accepted_jedec_ids": JEDEC,
                "flash_geometry": GEOMETRY,
                "operations": ["verify", "preflight", "dry-run", "flash"],
                "transport_integrity": ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"],
                "adaptive_transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
                "load_address": 0x81000000,
                "entry_address": 0x81000000,
                "entry_contract": "flat-binary-byte-zero-v1",
                "manifest_lookup_contract": "direct-object-members-v1",
                "hardware_preflight_contract": "spi-nor-scratch-rw-restore-loader-crc-v4",
                "spi_master_enable_contract": "preserve-general-ctrl-enable-spi-v1",
                "preflight_scratch": {
                    "default_address": 0x00FF0000,
                    "bytes": 0x10000,
                    "minimum_address": 0x40000,
                    "restore_original": True,
                },
                "binary": {
                    "filename": payload.name,
                    "bytes": payload.stat().st_size,
                    "sha256": digest,
                },
            }
            (self.recovery / f"recovery-{family}.descriptor.json").write_text(
                json.dumps(descriptor, indent=2, sort_keys=True) + "\n"
            )

        loader_sha = hashlib.sha256(image[:LOADER_BYTES]).hexdigest()
        self.loader_manifest = self.root / "loader.manifest.json"
        self.loader_manifest.write_text(json.dumps({
            "format": "postmerkos.vcoreiii-linuxloader-build.v7",
            "variant": "development",
            "boot_region": {"sha256": loader_sha, "size": LOADER_BYTES},
            "policies": {
                "crc": "warn", "size": "legacy-warn",
                "payload_slot_end": 0x300000, "hard_payload_limit": 0x2BFFE0,
            },
            "toolchain": {"id": "fixture-gcc473"},
            "uart_ramloader": {
                "enabled": True,
                "protocol_version": 2,
                "probe_timeout_ms": 3000,
                "interbyte_timeout_ms": 3000,
                "menu_selection_timeout_ms": 5000,
                "maximum_payload_bytes": 4 * 1024 * 1024,
                "ram_start": 0x81000000,
                "ram_end": 0x87F00000,
                "supported_soc_families": ["luton26", "jaguar1"],
                "transport_integrity": ["frame-crc32", "object-crc32", "object-sha256"],
                "boot_menu": {
                    "probe_timeout_ms": 3000, "selection_timeout_ms": 5000,
                    "options": {"1": "uart-ramloader", "2": "embedded-firmware-recovery"},
                    "noise_behavior": "invalid/no explicit option continues normal boot",
                },
                "image_check_diagnostics": "structured-pass-warn-fail-skip-values-v1",
                "embedded_recovery": embedded,
            },
        }, indent=2) + "\n")
        self.loader_version = self.root / "loader.version"
        self.loader_version.write_text("0.7.0\n")
        self.loader_revision = self.root / "loader.revision"
        self.loader_revision.write_text("fixture-revision\n")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_finalizer(self, *, expect_success: bool) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [
                sys.executable, str(SCRIPT), str(self.source), str(self.output),
                str(self.image), str(self.rootfs), str(self.loader_manifest), str(self.recovery),
                str(self.loader_version), str(self.loader_revision),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if expect_success and result.returncode != 0:
            self.fail(f"manifest finalizer failed: {result.stderr or result.stdout}")
        if not expect_success and result.returncode == 0:
            self.fail("manifest finalizer unexpectedly accepted an invalid fixture")
        return result

    def test_complete_contract_is_embedded(self) -> None:
        self.run_finalizer(expect_success=True)
        manifest = json.loads(self.output.read_text())
        self.assertEqual(manifest["artifact"]["bytes"], TOTAL_BYTES)
        self.assertEqual(manifest["artifact"]["boot_chain"], "vcoreiii-linuxloader-spim-v2")
        loader = manifest["recovery"]["uart_ramloader"]
        self.assertEqual(loader["protocol_version"], 2)
        self.assertEqual(loader["build_manifest_format"], "postmerkos.vcoreiii-linuxloader-build.v7")
        self.assertEqual(loader["boot_menu"]["options"]["2"], "embedded-firmware-recovery")
        self.assertEqual(manifest["artifact"]["bootloader"]["version"], "0.7.0")
        self.assertEqual(manifest["artifact"]["kernel_payload"]["alignment_bytes"], 32)
        firmware = manifest["recovery"]["uart_firmware"]
        self.assertEqual(firmware["flash_geometry"], GEOMETRY)
        self.assertEqual(firmware["accepted_jedec_ids"], JEDEC)
        self.assertEqual(firmware["operations"], ["verify", "preflight", "dry-run", "flash"])
        self.assertEqual(firmware["protocol_version"], 3)
        self.assertEqual(firmware["hardware_preflight_contract"], "spi-nor-scratch-rw-restore-loader-crc-v4")
        self.assertEqual(firmware["adaptive_transport_contract"], "pmosrec-v3-adaptive-uart-sparse-lz4-v1")
        self.assertEqual(firmware["spi_master_enable_contract"], "preserve-general-ctrl-enable-spi-v1")
        self.assertEqual(firmware["preflight_scratch"]["default_address"], 0x00FF0000)
        for family, target in TARGETS.items():
            record = firmware["payloads"][family]
            self.assertEqual(record["accepted_models"], target["models"])
            self.assertEqual(record["soc_family_id"], target["id"])
            self.assertEqual(record["spi_software_mode_address"], target["spi"])
            self.assertEqual(record["load_address"], 0x81000000)
            self.assertEqual(record["entry_address"], 0x81000000)
            self.assertEqual(record["entry_contract"], "flat-binary-byte-zero-v1")
            self.assertEqual(record["manifest_lookup_contract"], "direct-object-members-v1")
            self.assertEqual(record["hardware_preflight_contract"], "spi-nor-scratch-rw-restore-loader-crc-v4")
            self.assertEqual(record["spi_master_enable_contract"], "preserve-general-ctrl-enable-spi-v1")
            self.assertEqual(record["adaptive_transport_contract"], "pmosrec-v3-adaptive-uart-sparse-lz4-v1")
            self.assertTrue(record["preflight_scratch"]["restore_original"])

    def test_tampered_payload_is_rejected(self) -> None:
        payload = self.recovery / "recovery-jaguar1.bin"
        data = bytearray(payload.read_bytes())
        data[0] ^= 0x01
        payload.write_bytes(data)
        result = self.run_finalizer(expect_success=False)
        self.assertIn("digest does not match", result.stderr)

    def test_wrong_family_model_partition_is_rejected(self) -> None:
        descriptor_path = self.recovery / "recovery-luton26.descriptor.json"
        descriptor = json.loads(descriptor_path.read_text())
        descriptor["accepted_models"].append("MS42P")
        descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n")
        result = self.run_finalizer(expect_success=False)
        self.assertIn("model allow-list is invalid", result.stderr)


    def test_missing_hardware_preflight_contract_is_rejected(self) -> None:
        descriptor_path = self.recovery / "recovery-jaguar1.descriptor.json"
        descriptor = json.loads(descriptor_path.read_text())
        descriptor.pop("hardware_preflight_contract")
        descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n")
        result = self.run_finalizer(expect_success=False)
        self.assertIn("hardware preflight contract", result.stderr)

    def test_bootloader_overlapping_scratch_contract_is_rejected(self) -> None:
        descriptor_path = self.recovery / "recovery-luton26.descriptor.json"
        descriptor = json.loads(descriptor_path.read_text())
        descriptor["preflight_scratch"]["minimum_address"] = 0
        descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n")
        result = self.run_finalizer(expect_success=False)
        self.assertIn("scratch", result.stderr)

    def test_missing_embedded_descriptor_is_rejected(self) -> None:
        payload = self.recovery / "recovery-luton26.bin"
        data = payload.read_bytes().replace(b"PMOSRECOVERY3", b"PMOSRECOVERX3")
        payload.write_bytes(data)
        descriptor_path = self.recovery / "recovery-luton26.descriptor.json"
        descriptor = json.loads(descriptor_path.read_text())
        descriptor["binary"]["sha256"] = hashlib.sha256(data).hexdigest()
        descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n")
        result = self.run_finalizer(expect_success=False)
        self.assertIn("embedded target descriptor mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
