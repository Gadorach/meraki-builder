#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
need make
need python3
need sha256sum

"$SCRIPT_DIR/prepare-loader-source.sh"

variant="$LOADER_VARIANT"
build_mode="$LOADER_BUILD_MODE"
if [[ "${MS42P_IN_DISTROBOX:-0}" == 1 && "$build_mode" == auto ]]; then
  build_mode=native
fi

log "Building meraki-redboot $variant from source with the postmerkOS 0x00300000 kernel boundary"
run_logged meraki-redboot-build \
  make -C "$LOADER_SOURCE_DIR" -j"$JOBS" all \
    BUILD_MODE="$build_mode" \
    WORK_ROOT="$LOADER_WORK_DIR" \
    VARIANT="$variant" \
    UART_RAMLOADER=1 \
    PAYLOAD_SLOT_END="$LOADER_PAYLOAD_SLOT_END" \
    HARD_PAYLOAD_LIMIT="$LOADER_HARD_PAYLOAD_LIMIT" \
    LEGACY_PAYLOAD_LIMIT="$LOADER_HARD_PAYLOAD_LIMIT" \
    UART_RAMLOADER_PROBE_TIMEOUT_MS="${UART_RAMLOADER_PROBE_TIMEOUT_MS:-3000}" \
    UART_RAMLOADER_INTERBYTE_TIMEOUT_MS="${UART_RAMLOADER_INTERBYTE_TIMEOUT_MS:-3000}" \
    UART_MENU_TIMEOUT_MS="${UART_MENU_TIMEOUT_MS:-5000}"

source_image="$LOADER_WORK_DIR/artifacts/vcoreiii-linuxloader-$variant.bin"
source_manifest="$source_image.manifest.json"
source_recovery="$LOADER_WORK_DIR/recovery/artifacts"
source_liveboot="$LOADER_WORK_DIR/liveboot/artifacts"
[[ -f "$source_image" && -f "$source_manifest" ]] || die "meraki-redboot build did not produce its boot image and manifest"

mkdir -p "$ARTIFACTS_DIR" "$RECOVERY_ARTIFACT_DIR" "$LIVEBOOT_ARTIFACT_DIR" "$ARTIFACTS_DIR/tools"
cp -f "$source_image" "$LOADER_ARTIFACT"
cp -f "$source_manifest" "$LOADER_MANIFEST"
cp -f "$LOADER_PAYLOAD_PACKER" "$ARTIFACTS_DIR/tools/mkvcoreiii_payload.py"
chmod 0755 "$ARTIFACTS_DIR/tools/mkvcoreiii_payload.py"
write_sha256_sidecar "$LOADER_ARTIFACT"
write_sha256_sidecar "$LOADER_MANIFEST"
write_sha256_sidecar "$ARTIFACTS_DIR/tools/mkvcoreiii_payload.py"
[[ -f "$LOADER_SOURCE_SELECTION_RECORD" ]] || die "meraki-redboot source provenance record is missing: $LOADER_SOURCE_SELECTION_RECORD"
python3 - "$LOADER_BUILD_SOURCE_RECORD" "$LOADER_SOURCE_SELECTION_RECORD" \
  "$LOADER_SOURCE_VERSION_FILE" "$LOADER_SOURCE_REVISION_FILE" "$variant" <<'PY_SOURCE'
import json, sys
from pathlib import Path
out, selection_path, version_path, revision_path, variant = sys.argv[1:]
selection = json.loads(Path(selection_path).read_text())
required = ("repository", "requested_ref", "resolved_ref", "revision", "describe", "resolution")
missing = [key for key in required if not selection.get(key)]
if missing:
    raise SystemExit("meraki-redboot source provenance record is missing: " + ", ".join(missing))
revision = Path(revision_path).read_text().strip()
if selection["revision"] != revision:
    raise SystemExit("meraki-redboot source provenance revision does not match selected source")
selection.update({
    "version": Path(version_path).read_text().strip(),
    "variant": variant,
})
Path(out).write_text(json.dumps(selection, indent=2, sort_keys=True) + "\n")
PY_SOURCE
write_sha256_sidecar "$LOADER_BUILD_SOURCE_RECORD"

for name in \
  recovery-luton26.bin recovery-luton26.bin.sha256 recovery-luton26.descriptor.json \
  recovery-jaguar1.bin recovery-jaguar1.bin.sha256 recovery-jaguar1.descriptor.json; do
  [[ -f "$source_recovery/$name" ]] || die "Recovery build output is missing: $name"
  cp -f "$source_recovery/$name" "$RECOVERY_ARTIFACT_DIR/$name"
done
for name in pmoslive-jaguar1.bin pmoslive-jaguar1.bin.sha256 pmoslive-jaguar1.descriptor.json; do
  [[ -f "$source_liveboot/$name" ]] || die "PMOSLIVE build output is missing: $name"
  cp -f "$source_liveboot/$name" "$LIVEBOOT_ARTIFACT_DIR/$name"
done

python3 - "$LOADER_ARTIFACT" "$LOADER_MANIFEST" "$RECOVERY_ARTIFACT_DIR" "$LIVEBOOT_ARTIFACT_DIR" \
  "$LOADER_SOURCE_VERSION_FILE" "$LOADER_SOURCE_REVISION_FILE" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys

image, manifest_path, recovery_dir, liveboot_dir, version_path, revision_path = map(Path, sys.argv[1:])
data = image.read_bytes()
if len(data) != 0x40000:
    raise SystemExit("meraki-redboot boot region must be exactly 256 KiB")
for marker in (b"PMOSRAM READY 2", b"PMOSBOOT MENU-PROBE", b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY 3=LIVEBOOT"):
    if marker not in data:
        raise SystemExit(f"meraki-redboot image is missing capability marker: {marker!r}")
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
if manifest.get("format") != "postmerkos.vcoreiii-linuxloader-build.v7":
    raise SystemExit("unsupported meraki-redboot build manifest format")
cap = manifest.get("uart_ramloader", {})
policies = manifest.get("policies", {})
boot_menu = cap.get("boot_menu", {})
if cap.get("enabled") is not True or cap.get("protocol_version") != 2:
    raise SystemExit("meraki-redboot manifest does not declare UART RAM-loader v2")
if boot_menu.get("options") != {"1": "uart-ramloader", "2": "embedded-firmware-recovery", "3": "embedded-liveboot"}:
    raise SystemExit("meraki-redboot manifest does not declare the v0.7.0 boot menu")
if cap.get("image_check_diagnostics") != "structured-pass-warn-fail-skip-values-v1":
    raise SystemExit("meraki-redboot manifest lacks structured image diagnostics")
if cap.get("stage1_flash_offset") != 0x00020000 or cap.get("stage1_storage_contract") != "single-shared-boot-region-blob-v1":
    raise SystemExit("meraki-redboot manifest lacks the shared stage-1 boot-region contract")
if policies.get("payload_slot_end") != 0x300000 or policies.get("hard_payload_limit") != 0x2BFFE0:
    raise SystemExit("meraki-redboot was not built for the postmerkOS kernel slot")
if manifest.get("boot_region", {}).get("sha256") != hashlib.sha256(data).hexdigest():
    raise SystemExit("meraki-redboot manifest digest does not match loader1.bin")
if not version_path.read_text().strip() or not revision_path.read_text().strip():
    raise SystemExit("meraki-redboot source provenance files are empty")
expected = {
    "luton26": (1, 0x70000064, ["MS22", "MS22P", "MS220-8", "MS220-8P", "MS220-24", "MS220-24P"]),
    "jaguar1": (2, 0x70000068, [
        "MS320-24", "MS320-24P", "MS220-48", "MS220-48P", "MS220-48LP",
        "MS220-48FP", "MS320-48", "MS320-48P", "MS320-48LP", "MS320-48FP", "MS42", "MS42P",
    ]),
}
for family, (family_id, spi, models) in expected.items():
    payload = recovery_dir / f"recovery-{family}.bin"
    descriptor_path = recovery_dir / f"recovery-{family}.descriptor.json"
    raw = payload.read_bytes()
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    marker = f"PMOSRECOVERY3;SOC={family};FAMILY={family_id};SPI={spi:08x};PROTO=3;PREFLIGHT=4;BAUDTEST=1;FRAME_MAX=4096;WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;CONFIRM_RETRY=1;AUTO_CONFIRM=1;AUTO_REBOOT=1;END".encode()
    if raw.count(marker) != 1:
        raise SystemExit(f"{payload.name} has an invalid embedded target descriptor")
    if descriptor.get("format") != "postmerkos.uart-recovery-payload.v3":
        raise SystemExit(f"{payload.name} descriptor format is unsupported")
    if descriptor.get("protocol_version") != 3:
        raise SystemExit(f"{payload.name} descriptor protocol is not PMOSREC v3")
    if descriptor.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"{payload.name} lacks the PMOSREC v3 adaptive transport contract")
    if descriptor.get("transport_integrity") != ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"]:
        raise SystemExit(f"{payload.name} has an invalid PMOSREC v3 integrity contract")
    if descriptor.get("accepted_models") != models:
        raise SystemExit(f"{payload.name} descriptor model allow-list is invalid")
    if descriptor.get("load_address") != 0x86C00000 or descriptor.get("entry_address") != 0x86C00000:
        raise SystemExit(f"{payload.name} descriptor high-memory load/entry address is invalid")
    if descriptor.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"{payload.name} lacks the corrected flat-binary byte-zero entry contract")
    if descriptor.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise SystemExit(f"{payload.name} lacks scoped direct-member manifest parsing")
    if descriptor.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise SystemExit(f"{payload.name} lacks destructive SPI NOR preflight support")
    if descriptor.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise SystemExit(f"{payload.name} lacks the SPI master-enable correction")
    if descriptor.get("operations") != ["verify", "preflight", "dry-run", "flash"]:
        raise SystemExit(f"{payload.name} has an invalid operation contract")
    if descriptor.get("preflight_scratch") != {"default_address": 0x00FF0000, "bytes": 0x10000, "minimum_address": 0x40000, "restore_original": True}:
        raise SystemExit(f"{payload.name} has an invalid preflight scratch contract")
    embedded = cap.get("embedded_recovery", {}).get(family, {})
    if embedded.get("load_address") != 0x86C00000 or embedded.get("entry_address") != 0x86C00000:
        raise SystemExit(f"meraki-redboot embedded recovery high-memory address is invalid for {family}")
    if embedded.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks the corrected entry contract for {family}")
    if embedded.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks scoped manifest parsing for {family}")
    if embedded.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise SystemExit(f"meraki-redboot embedded recovery lacks hardware preflight for {family}")
    if embedded.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks SPI master-enable correction for {family}")
    if embedded.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks adaptive PMOSREC v3 transport for {family}")
    binary = descriptor.get("binary", {})
    if binary.get("bytes") != len(raw) or binary.get("sha256") != hashlib.sha256(raw).hexdigest():
        raise SystemExit(f"{payload.name} descriptor binary record mismatch")

live_payload = liveboot_dir / "pmoslive-jaguar1.bin"
live_descriptor_path = liveboot_dir / "pmoslive-jaguar1.descriptor.json"
live_raw = live_payload.read_bytes()
live_descriptor = json.loads(live_descriptor_path.read_text(encoding="utf-8"))
live_marker = (
    b"PMOSLIVE3;SOC=jaguar1;FAMILY=2;PROTO=3;FLASH=0;LIVEBOOT=1;"
    b"IMAGE_BYTES=16777216;KERNEL=81000000;ROOTFS=87000000;MEM_MIB=120;"
    b"FRAME_MAX=4096;WINDOW_MAX=16;SPARSE=1;LZ4=1;END"
)
if live_raw.count(live_marker) != 1:
    raise SystemExit("pmoslive-jaguar1.bin has an invalid embedded target descriptor")
if any(marker in live_raw for marker in (b"ERASEFLASH", b"FLASH-PREFLIGHT", b"PROGRESS ERASE", b"PROGRESS PROGRAM")):
    raise SystemExit("PMOSLIVE unexpectedly retains flash-write markers")
if live_descriptor.get("format") != "postmerkos.uart-liveboot-payload.v1" or live_descriptor.get("protocol_version") != 3:
    raise SystemExit("PMOSLIVE descriptor format/protocol is unsupported")
if live_descriptor.get("soc_family") != "jaguar1" or live_descriptor.get("accepted_models") != ["MS42", "MS42P"]:
    raise SystemExit("PMOSLIVE descriptor target allow-list is invalid")
if live_descriptor.get("operations") != ["verify", "dry-run", "liveboot"] or live_descriptor.get("flash_access") != "none":
    raise SystemExit("PMOSLIVE descriptor operation/flash policy is invalid")
if live_descriptor.get("load_address") != 0x86C00000 or live_descriptor.get("entry_address") != 0x86C00000:
    raise SystemExit("PMOSLIVE descriptor high-memory load/entry address is invalid")
if live_descriptor.get("transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
    raise SystemExit("PMOSLIVE descriptor transport contract is invalid")
if live_descriptor.get("linux_handoff") != "mips-legacy-argc-argv-envp-external-initrd-v1":
    raise SystemExit("PMOSLIVE descriptor Linux handoff is invalid")
if live_descriptor.get("rootfs_handoff") != "squashfs-as-legacy-initrd-v1":
    raise SystemExit("PMOSLIVE descriptor rootfs handoff is invalid")
ram = live_descriptor.get("ram_layout", {})
expected_ram = {
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
if ram != expected_ram:
    raise SystemExit("PMOSLIVE descriptor RAM layout is invalid")
live_binary = live_descriptor.get("binary", {})
live_digest = hashlib.sha256(live_raw).hexdigest()
if live_binary.get("bytes") != len(live_raw) or live_binary.get("sha256") != live_digest:
    raise SystemExit("PMOSLIVE descriptor binary record mismatch")
embedded_live = cap.get("embedded_liveboot", {}).get("jaguar1", {})
if embedded_live.get("size") != len(live_raw) or embedded_live.get("sha256") != live_digest:
    raise SystemExit("meraki-redboot embedded PMOSLIVE binding does not match the payload")
for key in (
    "load_address", "entry_address", "entry_contract", "flash_access",
    "accepted_models", "transport_contract", "linux_handoff",
    "rootfs_handoff", "kernel_load_address", "squashfs_address",
    "boot_params_physical_address", "boot_params_uncached_address",
    "boot_params_bytes", "linux_memory_mib", "top_reserved_mib",
):
    expected_value = {
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
    }[key]
    if embedded_live.get(key) != expected_value:
        raise SystemExit(f"meraki-redboot embedded PMOSLIVE {key} is invalid")
print("validated meraki-redboot capability, high-memory recovery, and flash-write-free PMOSLIVE payload")
PY

touch "$STAMP_DIR/loader-built"
log "meraki-redboot, recovery, and PMOSLIVE payloads are ready"
