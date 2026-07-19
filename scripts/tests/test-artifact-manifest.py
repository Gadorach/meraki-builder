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
        self.liveboot = self.root / "liveboot"
        self.liveboot.mkdir()

        image = bytearray(b"\xff" * TOTAL_BYTES)
        markers = (
            b"PMOSRAM READY 2",
            b"PMOSBOOT MENU-PROBE",
            b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT",
        )
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
                "load_address": 0x86C00000, "entry_address": 0x86C00000,
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
                "load_address": 0x86C00000,
                "entry_address": 0x86C00000,
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

        live_marker = (
            b"PMOSLIVE3;SOC=jaguar1;FAMILY=2;PROTO=3;FLASH=0;LIVEBOOT=1;"
            b"IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
            b"FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END"
        )
        self.live_payload = self.liveboot / "pmoslive-jaguar1.bin"
        self.live_payload.write_bytes(b"live-prefix\0" + live_marker + b"\0live-suffix")
        live_digest = hashlib.sha256(self.live_payload.read_bytes()).hexdigest()
        live_ram = {
            "kernel_load_address": 0x81000000,
            "image_staging_address": 0x81400000,
            "manifest_address": 0x82400000,
            "payload_address": 0x86C00000,
            "squashfs_address": 0x87000000,
            "boot_params_physical_address": 0x00000400,
            "boot_params_uncached_address": 0xA0000400,
            "boot_params_bytes": 0x00000C00,
            "linux_memory_mib": 120,
            "top_reserved_mib": 8,
        }
        live_descriptor = {
            "format": "postmerkos.uart-liveboot-payload.v1",
            "protocol_version": 3,
            "soc_family": "jaguar1",
            "soc_family_id": 2,
            "accepted_models": ["MS42", "MS42P"],
            "operations": ["verify", "dry-run", "liveboot"],
            "flash_access": "none",
            "load_address": 0x86C00000,
            "entry_address": 0x86C00000,
            "entry_contract": "flat-binary-byte-zero-v1",
            "transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
            "image": {
                "bytes": TOTAL_BYTES,
                "kernel_offset": KERNEL_OFFSET,
                "squashfs_offset": ROOTFS_OFFSET,
            },
            "ram_layout": live_ram,
            "linux_handoff": "mips-legacy-argc-argv-envp-external-initrd-v1",
            "platform_identity_handoff": "kernel-command-line-postmerkos-model-v1",
            "rootfs_handoff": "squashfs-as-legacy-initrd-v1",
            "binary": {
                "filename": self.live_payload.name,
                "bytes": self.live_payload.stat().st_size,
                "sha256": live_digest,
            },
        }
        (self.liveboot / "pmoslive-jaguar1.descriptor.json").write_text(
            json.dumps(live_descriptor, indent=2, sort_keys=True) + "\n"
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
                    "options": {
                        "1": "uart-ramloader",
                        "2": "embedded-firmware-recovery",
                        "3": "embedded-liveboot",
                    },
                    "noise_behavior": "invalid/no explicit option continues normal boot",
                },
                "image_check_diagnostics": "structured-pass-warn-fail-skip-values-v1",
                "stage1_flash_offset": 0x00020000,
                "stage1_storage_contract": "single-shared-boot-region-blob-v1",
                "embedded_recovery": embedded,
                "embedded_liveboot": {
                    "jaguar1": {
                        "path": str(self.live_payload),
                        "size": self.live_payload.stat().st_size,
                        "sha256": live_digest,
                        "load_address": 0x86C00000,
                        "entry_address": 0x86C00000,
                        "entry_contract": "flat-binary-byte-zero-v1",
                        "flash_access": "none",
                        "accepted_models": ["MS42", "MS42P"],
                        "transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
                        "linux_handoff": "mips-legacy-argc-argv-envp-external-initrd-v1",
                        "platform_identity_handoff": "kernel-command-line-postmerkos-model-v1",
                        "rootfs_handoff": "squashfs-as-legacy-initrd-v1",
                        "kernel_load_address": 0x81000000,
                        "squashfs_address": 0x87000000,
                        "boot_params_physical_address": 0x00000400,
                        "boot_params_uncached_address": 0xA0000400,
                        "boot_params_bytes": 0x00000C00,
                        "linux_memory_mib": 120,
                        "top_reserved_mib": 8,
                    },
                },
            },
        }, indent=2) + "\n")
        self.loader_version = self.root / "loader.version"
        self.loader_version.write_text("0.7.0\n")
        self.loader_revision = self.root / "loader.revision"
        self.loader_revision.write_text("fixture-revision\n")
        self.kernel_contract = self.root / "pmoslive-kernel-contract.json"
        required_config = {
            "CONFIG_BLOCK": "y",
            "CONFIG_BLK_DEV": "y",
            "CONFIG_BLK_DEV_INITRD": "y",
            "CONFIG_BLK_DEV_RAM": "y",
            "CONFIG_BLK_DEV_RAM_COUNT": "1",
            "CONFIG_BLK_DEV_RAM_SIZE": "16384",
            "CONFIG_RD_XZ": "y",
            "CONFIG_SQUASHFS": "y",
            "CONFIG_SQUASHFS_XZ": "y",
            "CONFIG_XZ_DEC": "y",
            "CONFIG_DECOMPRESS_XZ": "y",
            "CONFIG_CMDLINE_BOOL": "y",
            "CONFIG_CMDLINE_OVERRIDE": "n",
            "CONFIG_CMDLINE_FALLBACK": "y",
        }
        self.kernel_contract.write_text(json.dumps({
            "format": "postmerkos.kernel-build-contract.v1",
            "kernel_version": "3.18.123",
            "boot_argument_contract": "vcoreiii-standard-mips-argc-argv-envp-fallback-v1",
            "source_revision": "fixture-kernel-revision",
            "managed_patch": {"filename": "fixture.patch", "sha256": "0" * 64},
            "config_policy": {
                "filename": "configure-liveboot-kernel.py",
                "sha256": "1" * 64,
                "required": dict(sorted(required_config.items())),
            },
            "resolved_config": dict(sorted(required_config.items())),
            "artifacts": {
                "vmlinuz": {"filename": "vmlinuz", "bytes": len(kernel), "sha256": hashlib.sha256(kernel).hexdigest()},
                "vmlinuz_bin": {"filename": "vmlinuz.bin", "bytes": len(kernel), "sha256": hashlib.sha256(kernel).hexdigest()},
                "headers": {"filename": "linux-3.18.123.tar.bz2", "bytes": 1, "sha256": "2" * 64},
            },
        }, indent=2, sort_keys=True) + "\n")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_finalizer(self, *, expect_success: bool) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [
                sys.executable, str(SCRIPT), str(self.source), str(self.output),
                str(self.image), str(self.rootfs), str(self.loader_manifest), str(self.recovery),
                str(self.liveboot), str(self.kernel_contract),
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
        self.assertEqual(loader["boot_menu"]["options"]["3"], "embedded-liveboot")
        self.assertEqual(loader["stage1_flash_offset"], 0x00020000)
        self.assertEqual(loader["stage1_storage_contract"], "single-shared-boot-region-blob-v1")
        self.assertEqual(manifest["artifact"]["bootloader"]["version"], "0.7.0")
        self.assertEqual(manifest["artifact"]["kernel_payload"]["alignment_bytes"], 32)
        self.assertEqual(
            manifest["artifact"]["kernel_payload"]["boot_argument_contract"],
            "vcoreiii-standard-mips-argc-argv-envp-fallback-v1",
        )
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
            self.assertEqual(record["load_address"], 0x86C00000)
            self.assertEqual(record["entry_address"], 0x86C00000)
            self.assertEqual(record["entry_contract"], "flat-binary-byte-zero-v1")
            self.assertEqual(record["manifest_lookup_contract"], "direct-object-members-v1")
            self.assertEqual(record["hardware_preflight_contract"], "spi-nor-scratch-rw-restore-loader-crc-v4")
            self.assertEqual(record["spi_master_enable_contract"], "preserve-general-ctrl-enable-spi-v1")
            self.assertEqual(record["adaptive_transport_contract"], "pmosrec-v3-adaptive-uart-sparse-lz4-v1")
            self.assertTrue(record["preflight_scratch"]["restore_original"])
        live = manifest["recovery"]["uart_liveboot"]
        self.assertEqual(live["flash_access"], "none")
        self.assertEqual(live["operations"], ["verify", "dry-run", "liveboot"])
        live_record = live["payloads"]["jaguar1"]
        self.assertEqual(live_record["load_address"], 0x86C00000)
        self.assertEqual(live_record["ram_layout"]["squashfs_address"], 0x87000000)
        embedded_live = loader["embedded_liveboot"]["jaguar1"]
        self.assertEqual(embedded_live["kernel_load_address"], 0x81000000)
        self.assertEqual(embedded_live["squashfs_address"], 0x87000000)
        self.assertEqual(embedded_live["boot_params_physical_address"], 0x00000400)
        self.assertEqual(embedded_live["boot_params_uncached_address"], 0xA0000400)
        self.assertEqual(embedded_live["boot_params_bytes"], 0x00000C00)
        self.assertEqual(embedded_live["linux_memory_mib"], 120)
        self.assertEqual(embedded_live["top_reserved_mib"], 8)

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


    def test_stale_kernel_contract_is_rejected(self) -> None:
        contract = json.loads(self.kernel_contract.read_text())
        contract["artifacts"]["vmlinuz_bin"]["sha256"] = "f" * 64
        self.kernel_contract.write_text(json.dumps(contract, indent=2) + "\n")
        result = self.run_finalizer(expect_success=False)
        self.assertIn("does not match", result.stderr)



if __name__ == "__main__":
    unittest.main(verbosity=2)
