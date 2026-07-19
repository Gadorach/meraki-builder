#!/usr/bin/env python3
"""Finalize and validate a postmerkOS manifest for a published firmware artifact."""
from __future__ import annotations

import hashlib
import json
import re
import struct
import zlib
from pathlib import Path
import sys

LOADER_BYTES = 0x040000
KERNEL_BYTES = 0x2C0000
ROOTFS_BYTES = 0x800000
OVERLAY_BYTES = 0x500000
TOTAL_BYTES = 0x1000000
BOOT_CHAIN = "vcoreiii-linuxloader-spim-v2"
SPIM_HEADER = struct.Struct("<8I")
SPIM_MAGIC = 0x4D495053


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) not in (8, 10):
        print(
            "usage: write-artifact-manifest.py INPUT.json OUTPUT.json IMAGE ROOTFS "
            "LOADER-MANIFEST.json RECOVERY-ARTIFACT-DIR LIVEBOOT-ARTIFACT-DIR "
            "[LOADER-VERSION LOADER-REVISION]",
            file=sys.stderr,
        )
        return 2
    source, output, image, rootfs, loader_manifest_path, recovery_dir, liveboot_dir = map(Path, argv[1:8])
    loader_version_path = Path(argv[8]) if len(argv) == 10 else None
    loader_revision_path = Path(argv[9]) if len(argv) == 10 else None
    manifest = json.loads(source.read_text(encoding="utf-8"))
    loader_manifest = json.loads(loader_manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or not isinstance(loader_manifest, dict):
        raise SystemExit("release and loader manifests must contain JSON objects")
    for path in (image, rootfs):
        if not path.is_file():
            raise SystemExit(f"required artifact is missing: {path}")
    image_size = image.stat().st_size
    rootfs_size = rootfs.stat().st_size
    if image_size != TOTAL_BYTES:
        raise SystemExit(f"firmware image must be exactly {TOTAL_BYTES} bytes")
    if not 0 < rootfs_size <= ROOTFS_BYTES:
        raise SystemExit(f"SquashFS must be 1..{ROOTFS_BYTES} bytes")

    image_data = image.read_bytes()
    loader_data = image_data[:LOADER_BYTES]
    uart = loader_manifest.get("uart_ramloader", {})
    loader_record = loader_manifest.get("boot_region", {})
    if uart.get("enabled") is not True or uart.get("protocol_version") != 2:
        raise SystemExit("loader manifest does not declare UART RAM-loader v2")
    loader_digest = hashlib.sha256(loader_data).hexdigest()
    if loader_record.get("sha256") != loader_digest:
        raise SystemExit("published image loader region does not match the source-built loader manifest")
    for marker in (b"PMOSRAM READY 2", b"PMOSBOOT MENU-PROBE", b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT"):
        if marker not in loader_data:
            raise SystemExit(f"published image loader region is missing meraki-redboot capability marker {marker!r}")
    if image_data[LOADER_BYTES:LOADER_BYTES + 4] != b"SPIM":
        raise SystemExit("published image kernel region is missing the SPIM header")
    if image_data[LOADER_BYTES + KERNEL_BYTES:LOADER_BYTES + KERNEL_BYTES + 4] != b"hsqs":
        raise SystemExit("published image rootfs region is missing the SquashFS header")

    header = image_data[LOADER_BYTES:LOADER_BYTES + SPIM_HEADER.size]
    magic, load, payload_size, entry, stored_crc, r0, r1, r2 = SPIM_HEADER.unpack(header)
    if magic != SPIM_MAGIC or load != 0x81000000 or entry != 0x81000000:
        raise SystemExit("published image has an invalid meraki-redboot SPIM address/header contract")
    if not 0 < payload_size <= KERNEL_BYTES - SPIM_HEADER.size or payload_size % 32:
        raise SystemExit("published image SPIM payload size is outside the aligned kernel slot")
    if (r0, r1, r2) != (0, 0, 0):
        raise SystemExit("published image SPIM reserved words are non-zero")
    payload = image_data[LOADER_BYTES + SPIM_HEADER.size:LOADER_BYTES + SPIM_HEADER.size + payload_size]
    zeroed_header = bytearray(header)
    struct.pack_into("<I", zeroed_header, 16, 0)
    calculated_crc = zlib.crc32(zeroed_header + payload) & 0xFFFFFFFF
    if stored_crc != calculated_crc:
        raise SystemExit(
            f"published image SPIM CRC mismatch: stored 0x{stored_crc:08x}, calculated 0x{calculated_crc:08x}"
        )

    loader_format = loader_manifest.get("format")
    if loader_format != "postmerkos.vcoreiii-linuxloader-build.v7":
        raise SystemExit("loader manifest is not the meraki-redboot v0.7 capability format")
    policies = loader_manifest.get("policies", {})
    if policies.get("payload_slot_end") != 0x300000 or policies.get("hard_payload_limit") != 0x2BFFE0:
        raise SystemExit("loader manifest is not built for the postmerkOS kernel slot")
    expected_menu = {"1": "uart-ramloader", "2": "embedded-firmware-recovery", "3": "embedded-liveboot"}
    if uart.get("boot_menu", {}).get("options") != expected_menu:
        raise SystemExit("loader manifest does not declare the meraki-redboot v0.7 boot menu")
    if uart.get("image_check_diagnostics") != "structured-pass-warn-fail-skip-values-v1":
        raise SystemExit("loader manifest does not declare structured image diagnostics")
    if uart.get("stage1_flash_offset") != 0x00020000 or uart.get("stage1_storage_contract") != "single-shared-boot-region-blob-v1":
        raise SystemExit("loader manifest does not declare the shared stage-1 boot-region contract")
    embedded_source = uart.get("embedded_recovery")
    if not isinstance(embedded_source, dict):
        raise SystemExit("loader manifest does not bind embedded platform recovery payloads")
    if loader_version_path is not None and not loader_version_path.is_file():
        raise SystemExit(f"loader version record is missing: {loader_version_path}")
    if loader_revision_path is not None and not loader_revision_path.is_file():
        raise SystemExit(f"loader revision record is missing: {loader_revision_path}")

    manifest["target_family"] = "vcore3"
    manifest["image_format"] = max(int(manifest.get("image_format", 0) or 0), 2)
    recovery_payloads = {}
    common_geometry = None
    common_jedec = None
    expected = {
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
    expected_geometry = {"bytes": TOTAL_BYTES, "erase_bytes": 64 * 1024, "page_bytes": 256, "address_bytes": 3}
    known_models = set(manifest.get("models", {}))
    for family in ("luton26", "jaguar1"):
        descriptor_path = recovery_dir / f"recovery-{family}.descriptor.json"
        if not descriptor_path.is_file():
            raise SystemExit(f"recovery descriptor is missing: {descriptor_path}")
        descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
        binary_record = descriptor.get("binary", {})
        binary_path = recovery_dir / str(binary_record.get("filename", ""))
        if descriptor.get("format") != "postmerkos.uart-recovery-payload.v3":
            raise SystemExit(f"unsupported recovery descriptor format: {descriptor_path}")
        if descriptor.get("soc_family") != family or descriptor.get("protocol_version") != 3:
            raise SystemExit(f"recovery descriptor family/protocol mismatch: {descriptor_path}")
        if descriptor.get("soc_family_id") != expected[family]["id"] or descriptor.get("spi_software_mode_address") != expected[family]["spi"]:
            raise SystemExit(f"recovery descriptor target register mismatch: {descriptor_path}")
        if descriptor.get("accepted_flash_bytes") != TOTAL_BYTES or descriptor.get("flash_geometry") != expected_geometry:
            raise SystemExit(f"recovery descriptor flash geometry mismatch: {descriptor_path}")
        if descriptor.get("operations") != ["verify", "preflight", "dry-run", "flash"]:
            raise SystemExit(f"recovery descriptor operation contract mismatch: {descriptor_path}")
        if descriptor.get("load_address") != 0x86C00000 or descriptor.get("entry_address") != 0x86C00000:
            raise SystemExit(f"recovery descriptor high-memory load/entry address mismatch: {descriptor_path}")
        if descriptor.get("entry_contract") != "flat-binary-byte-zero-v1":
            raise SystemExit(f"recovery descriptor lacks corrected byte-zero entry contract: {descriptor_path}")
        if descriptor.get("manifest_lookup_contract") != "direct-object-members-v1":
            raise SystemExit(f"recovery descriptor lacks direct-member manifest lookup: {descriptor_path}")
        if descriptor.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
            raise SystemExit(f"recovery descriptor lacks hardware preflight contract: {descriptor_path}")
        if descriptor.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
            raise SystemExit(f"recovery descriptor lacks SPI master-enable correction: {descriptor_path}")
        if descriptor.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
            raise SystemExit(f"recovery descriptor lacks PMOSREC v3 adaptive transport: {descriptor_path}")
        expected_scratch = {
            "default_address": 0x00FF0000,
            "bytes": 0x10000,
            "minimum_address": LOADER_BYTES,
            "restore_original": True,
        }
        if descriptor.get("preflight_scratch") != expected_scratch:
            raise SystemExit(f"recovery descriptor preflight scratch contract mismatch: {descriptor_path}")
        if descriptor.get("transport_integrity") != ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"]:
            raise SystemExit(f"recovery descriptor integrity contract mismatch: {descriptor_path}")
        accepted_models = descriptor.get("accepted_models")
        if accepted_models != expected[family]["models"] or any(model not in known_models for model in accepted_models):
            raise SystemExit(f"recovery descriptor model allow-list is invalid: {descriptor_path}")
        geometry = descriptor.get("flash_geometry")
        jedec = descriptor.get("accepted_jedec_ids")
        if not isinstance(jedec, list) or not jedec or any(
            not isinstance(item, str) or re.fullmatch(r"[0-9a-f]{6}", item) is None for item in jedec
        ):
            raise SystemExit(f"recovery descriptor JEDEC allow-list is invalid: {descriptor_path}")
        if not binary_path.is_file() or binary_path.stat().st_size != binary_record.get("bytes"):
            raise SystemExit(f"recovery payload size does not match descriptor: {binary_path}")
        payload_data = binary_path.read_bytes()
        marker = (
            f"PMOSRECOVERY3;SOC={family};FAMILY={expected[family]['id']};"
            f"SPI={expected[family]['spi']:08x};PROTO=3;PREFLIGHT=4;BAUDTEST=1;"
            "FRAME_MAX=4096;WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;"
            "CONFIRM_RETRY=1;AUTO_CONFIRM=1;AUTO_REBOOT=1;END"
        ).encode("ascii")
        if payload_data.count(marker) != 1:
            raise SystemExit(f"recovery payload embedded target descriptor mismatch: {binary_path}")
        digest = hashlib.sha256(payload_data).hexdigest()
        if digest != str(binary_record.get("sha256", "")).lower():
            raise SystemExit(f"recovery payload digest does not match descriptor: {binary_path}")
        embedded_record = embedded_source.get(family)
        if not isinstance(embedded_record, dict):
            raise SystemExit(f"loader manifest has no embedded recovery record for {family}")
        if embedded_record.get("size") != len(payload_data) or str(embedded_record.get("sha256", "")).lower() != digest:
            raise SystemExit(f"loader embedded recovery binding does not match {binary_path.name}")
        if embedded_record.get("load_address") != 0x86C00000 or embedded_record.get("entry_address") != 0x86C00000:
            raise SystemExit(f"loader embedded recovery high-memory load/entry address mismatch: {binary_path.name}")
        if embedded_record.get("entry_contract") != "flat-binary-byte-zero-v1":
            raise SystemExit(f"loader embedded recovery lacks corrected byte-zero entry contract: {binary_path.name}")
        if embedded_record.get("manifest_lookup_contract") != "direct-object-members-v1":
            raise SystemExit(f"loader embedded recovery lacks direct-member manifest lookup: {binary_path.name}")
        if embedded_record.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
            raise SystemExit(f"loader embedded recovery lacks hardware preflight support: {binary_path.name}")
        if embedded_record.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
            raise SystemExit(f"loader embedded recovery lacks SPI master-enable correction: {binary_path.name}")
        if embedded_record.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
            raise SystemExit(f"loader embedded recovery lacks PMOSREC v3 adaptive transport: {binary_path.name}")
        if common_geometry is None:
            common_geometry = geometry
            common_jedec = jedec
        elif geometry != common_geometry or jedec != common_jedec:
            raise SystemExit("recovery payload descriptors disagree on flash constraints")
        recovery_payloads[family] = {
            "filename": binary_path.name,
            "bytes": binary_path.stat().st_size,
            "sha256": digest,
            "soc_family_id": expected[family]["id"],
            "spi_software_mode_address": expected[family]["spi"],
            "accepted_models": list(accepted_models),
            "load_address": descriptor["load_address"],
            "entry_address": descriptor["entry_address"],
            "entry_contract": descriptor["entry_contract"],
            "manifest_lookup_contract": descriptor["manifest_lookup_contract"],
            "hardware_preflight_contract": descriptor["hardware_preflight_contract"],
            "spi_master_enable_contract": descriptor["spi_master_enable_contract"],
            "adaptive_transport_contract": descriptor["adaptive_transport_contract"],
            "adaptive_transport": descriptor.get("adaptive_transport"),
            "preflight_scratch": descriptor["preflight_scratch"],
        }

    live_descriptor_path = liveboot_dir / "pmoslive-jaguar1.descriptor.json"
    if not live_descriptor_path.is_file():
        raise SystemExit(f"PMOSLIVE descriptor is missing: {live_descriptor_path}")
    live_descriptor = json.loads(live_descriptor_path.read_text(encoding="utf-8"))
    live_binary_record = live_descriptor.get("binary", {})
    live_binary_path = liveboot_dir / str(live_binary_record.get("filename", ""))
    if not live_binary_path.is_file():
        raise SystemExit(f"PMOSLIVE binary is missing: {live_binary_path}")
    live_data = live_binary_path.read_bytes()
    live_digest = hashlib.sha256(live_data).hexdigest()
    live_marker = (
        b"PMOSLIVE3;SOC=jaguar1;FAMILY=2;PROTO=3;FLASH=0;LIVEBOOT=1;"
        b"IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
        b"FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END"
    )
    if live_data.count(live_marker) != 1:
        raise SystemExit("PMOSLIVE embedded target descriptor mismatch")
    if any(marker in live_data for marker in (b"ERASEFLASH", b"FLASH-PREFLIGHT", b"PROGRESS ERASE", b"PROGRESS PROGRAM")):
        raise SystemExit("PMOSLIVE unexpectedly contains flash-write markers")
    if live_descriptor.get("format") != "postmerkos.uart-liveboot-payload.v1" or live_descriptor.get("protocol_version") != 3:
        raise SystemExit("PMOSLIVE descriptor format/protocol mismatch")
    if live_descriptor.get("soc_family") != "jaguar1" or live_descriptor.get("soc_family_id") != 2:
        raise SystemExit("PMOSLIVE descriptor family mismatch")
    if live_descriptor.get("accepted_models") != ["MS42", "MS42P"]:
        raise SystemExit("PMOSLIVE descriptor model allow-list mismatch")
    if live_descriptor.get("operations") != ["verify", "dry-run", "liveboot"] or live_descriptor.get("flash_access") != "none":
        raise SystemExit("PMOSLIVE descriptor operation/flash contract mismatch")
    if live_descriptor.get("load_address") != 0x86C00000 or live_descriptor.get("entry_address") != 0x86C00000:
        raise SystemExit("PMOSLIVE descriptor high-memory load/entry mismatch")
    if live_descriptor.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit("PMOSLIVE descriptor entry contract mismatch")
    if live_descriptor.get("transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit("PMOSLIVE descriptor transport contract mismatch")
    if live_descriptor.get("linux_handoff") != "mips-legacy-argc-argv-envp-external-initrd-v1":
        raise SystemExit("PMOSLIVE descriptor Linux handoff mismatch")
    if live_descriptor.get("rootfs_handoff") != "squashfs-as-legacy-initrd-v1":
        raise SystemExit("PMOSLIVE descriptor rootfs handoff mismatch")
    live_image = live_descriptor.get("image", {})
    if live_image != {"bytes": TOTAL_BYTES, "kernel_offset": LOADER_BYTES, "squashfs_offset": LOADER_BYTES + KERNEL_BYTES}:
        raise SystemExit("PMOSLIVE descriptor image geometry mismatch")
    live_ram = live_descriptor.get("ram_layout", {})
    expected_live_ram = {
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
    if live_ram != expected_live_ram:
        raise SystemExit("PMOSLIVE descriptor RAM layout mismatch")
    if live_binary_record.get("bytes") != len(live_data) or str(live_binary_record.get("sha256", "")).lower() != live_digest:
        raise SystemExit("PMOSLIVE descriptor binary record mismatch")
    embedded_live_source = uart.get("embedded_liveboot", {}).get("jaguar1", {})
    if embedded_live_source.get("size") != len(live_data) or str(embedded_live_source.get("sha256", "")).lower() != live_digest:
        raise SystemExit("loader embedded PMOSLIVE binding does not match the standalone payload")
    expected_embedded_live = {
        "load_address": 0x86C00000,
        "entry_address": 0x86C00000,
        "entry_contract": "flat-binary-byte-zero-v1",
        "flash_access": "none",
        "accepted_models": ["MS42", "MS42P"],
        "transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
        "linux_handoff": "mips-legacy-argc-argv-envp-external-initrd-v1",
        "rootfs_handoff": "squashfs-as-legacy-initrd-v1",
        "kernel_load_address": 0x81000000,
        "squashfs_address": 0x87000000,
        "boot_params_physical_address": 0x00000400,
        "boot_params_uncached_address": 0xA0000400,
        "boot_params_bytes": 0x00000C00,
        "linux_memory_mib": 120,
        "top_reserved_mib": 8,
    }
    for key, value in expected_embedded_live.items():
        if embedded_live_source.get(key) != value:
            raise SystemExit(f"loader embedded PMOSLIVE {key} mismatch")
    live_payload = {
        "filename": live_binary_path.name,
        "bytes": len(live_data),
        "sha256": live_digest,
        "soc_family_id": 2,
        "accepted_models": ["MS42", "MS42P"],
        "load_address": 0x86C00000,
        "entry_address": 0x86C00000,
        "entry_contract": "flat-binary-byte-zero-v1",
        "flash_access": "none",
        "transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
        "linux_handoff": "mips-legacy-argc-argv-envp-external-initrd-v1",
        "rootfs_handoff": "squashfs-as-legacy-initrd-v1",
        "ram_layout": expected_live_ram,
    }

    manifest["recovery"] = {
        "uart_ramloader": {
            "enabled": True,
            "protocol_version": 2,
            "build_manifest_format": loader_format,
            "source_project": "Gadorach/meraki-redboot",
            "source_version": loader_version_path.read_text(encoding="utf-8").strip() if loader_version_path else None,
            "source_revision": loader_revision_path.read_text(encoding="utf-8").strip() if loader_revision_path else None,
            "probe_timeout_ms": int(uart["probe_timeout_ms"]),
            "interbyte_timeout_ms": int(uart["interbyte_timeout_ms"]),
            "menu_selection_timeout_ms": int(uart["menu_selection_timeout_ms"]),
            "maximum_payload_bytes": int(uart["maximum_payload_bytes"]),
            "ram_start": int(uart["ram_start"]),
            "ram_end": int(uart["ram_end"]),
            "supported_soc_families": list(uart["supported_soc_families"]),
            "transport_integrity": list(uart["transport_integrity"]),
            "boot_menu": uart["boot_menu"],
            "image_check_diagnostics": uart["image_check_diagnostics"],
            "stage1_flash_offset": int(uart["stage1_flash_offset"]),
            "stage1_storage_contract": uart["stage1_storage_contract"],
            "embedded_recovery": {
                family: {
                    "bytes": recovery_payloads[family]["bytes"],
                    "sha256": recovery_payloads[family]["sha256"],
                    "load_address": recovery_payloads[family]["load_address"],
                    "entry_address": recovery_payloads[family]["entry_address"],
                    "entry_contract": recovery_payloads[family]["entry_contract"],
                    "manifest_lookup_contract": recovery_payloads[family]["manifest_lookup_contract"],
                    "hardware_preflight_contract": recovery_payloads[family]["hardware_preflight_contract"],
                    "spi_master_enable_contract": recovery_payloads[family]["spi_master_enable_contract"],
                    "adaptive_transport_contract": recovery_payloads[family]["adaptive_transport_contract"],
                } for family in ("luton26", "jaguar1")
            },
            "embedded_liveboot": {"jaguar1": {
                **dict(live_payload),
                "kernel_load_address": expected_live_ram["kernel_load_address"],
                "squashfs_address": expected_live_ram["squashfs_address"],
                "boot_params_physical_address": expected_live_ram["boot_params_physical_address"],
                "boot_params_uncached_address": expected_live_ram["boot_params_uncached_address"],
                "boot_params_bytes": expected_live_ram["boot_params_bytes"],
                "linux_memory_mib": expected_live_ram["linux_memory_mib"],
                "top_reserved_mib": expected_live_ram["top_reserved_mib"],
            }},
            "loader_sha256": loader_digest,
        },
        "uart_firmware": {
            "enabled": True,
            "protocol_version": 3,
            "full_image_bytes": TOTAL_BYTES,
            "operations": ["verify", "preflight", "dry-run", "flash"],
            "hardware_preflight_contract": "spi-nor-scratch-rw-restore-loader-crc-v4",
            "spi_master_enable_contract": "preserve-general-ctrl-enable-spi-v1",
            "adaptive_transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
            "adaptive_transport": {
                "stable_bootstrap_baud": 115200,
                "target_divisor_negotiation": True,
                "bidirectional_prng_crc_test": True,
                "autonomous_baud_fallback": True,
                "baud_refinement_percent": 2,
                "frame_sizes": [1024, 4096],
                "maximum_window_frames": 16,
                "ack_format": "binary-cumulative-selective-retry-v1",
                "manifest_first": True,
                "representations": ["raw", "sparse", "lz4", "sparse-lz4"],
                "confirmation_retry": "infinite",
                "automatic_reboot_seconds": 5,
            },
            "preflight_scratch": {"default_address": 0x00FF0000, "bytes": 64 * 1024, "minimum_address": 0x00040000, "restore_original": True},
            "transport_integrity": ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"],
            "flash_geometry": common_geometry,
            "accepted_jedec_ids": common_jedec,
            "delivery": "meraki-redboot-stage1-menu-option-2",
            "legacy_delivery": "meraki-redboot-stage1-menu-option-1-ram-upload",
            "payloads": recovery_payloads,
        },
        "uart_liveboot": {
            "enabled": True,
            "protocol_version": 3,
            "full_image_bytes": TOTAL_BYTES,
            "operations": ["verify", "dry-run", "liveboot"],
            "flash_access": "none",
            "delivery": "meraki-redboot-stage1-menu-option-3",
            "legacy_delivery": "meraki-redboot-stage1-menu-option-1-ram-upload",
            "transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
            "transport_integrity": ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"],
            "linux_handoff": "mips-legacy-argc-argv-envp-external-initrd-v1",
            "rootfs_handoff": "squashfs-as-legacy-initrd-v1",
            "payloads": {"jaguar1": live_payload},
        },
    }
    manifest["artifact"] = {
        "filename": image.name,
        "bytes": image_size,
        "sha256": hashlib.sha256(image_data).hexdigest(),
        "boot_chain": BOOT_CHAIN,
        "rootfs_filename": rootfs.name,
        "rootfs_bytes": rootfs_size,
        "rootfs_sha256": sha256(rootfs),
        "supported_flash_scopes": ["system", "full"],
        "default_flash_scope": "system",
        "bootloader": {
            "project": "Gadorach/meraki-redboot",
            "source_built": True,
            "version": loader_version_path.read_text(encoding="utf-8").strip() if loader_version_path else None,
            "revision": loader_revision_path.read_text(encoding="utf-8").strip() if loader_revision_path else None,
            "build_manifest_format": loader_format,
            "variant": loader_manifest.get("variant"),
            "policies": policies,
            "toolchain": loader_manifest.get("toolchain"),
            "boot_menu": uart.get("boot_menu"),
            "image_check_diagnostics": uart.get("image_check_diagnostics"),
        },
        "kernel_payload": {
            "format": "postmerkos.vcoreiii-payload.v1",
            "header_bytes": SPIM_HEADER.size,
            "payload_bytes": payload_size,
            "alignment_bytes": 32,
            "load_address": load,
            "entry_point": entry,
            "crc32": f"{stored_crc:08x}",
            "sha256": hashlib.sha256(payload).hexdigest(),
        },
        "regions": {
            "bootloader": {"offset": 0, "bytes": LOADER_BYTES, "sha256": loader_digest},
            "kernel": {"offset": LOADER_BYTES, "bytes": KERNEL_BYTES, "payload_bytes": payload_size},
            "squashfs": {
                "offset": LOADER_BYTES + KERNEL_BYTES,
                "bytes": ROOTFS_BYTES,
                "payload_bytes": rootfs_size,
            },
            "jffs2": {
                "offset": LOADER_BYTES + KERNEL_BYTES + ROOTFS_BYTES,
                "bytes": OVERLAY_BYTES,
            },
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
