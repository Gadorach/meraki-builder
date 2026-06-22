#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
load_build_state
need readelf
need sha256sum
need python3

required=(
  "$KERNEL_ARTIFACT_DIR/vmlinuz"
  "$KERNEL_ARTIFACT_DIR/vmlinuz.bin"
  "$KERNEL_HEADERS_TARBALL"
  "$LOADER_ARTIFACT"
  "$LOADER_MANIFEST"
  "$LOADER_BUILD_SOURCE_RECORD"
  "$LOADER_SOURCE_REVISION_FILE"
  "$LOADER_SOURCE_VERSION_FILE"
  "$ARTIFACTS_DIR/tools/mkvcoreiii_payload.py"
  "$RECOVERY_ARTIFACT_DIR/recovery-luton26.bin"
  "$RECOVERY_ARTIFACT_DIR/recovery-luton26.descriptor.json"
  "$RECOVERY_ARTIFACT_DIR/recovery-jaguar1.bin"
  "$RECOVERY_ARTIFACT_DIR/recovery-jaguar1.descriptor.json"
  "$DONOR_ROOT/lib/modules/postmerkos-required-modules.txt"
  "$DONOR_ROOT/lib/modules/postmerkos-all-modules.txt"
  "$DONOR_ROOT/lib/modules/postmerkos-modules.sha256"
)
if bool_enabled "${INCLUDE_UI:-0}"; then
  required+=("$BUILD_DIR/postmerkos-ui/index.html")
fi
for input in "${required[@]}"; do
  [[ -f "$input" ]] || die "Required build input is missing: $input"
done
verify_vendor_module_tree "$DONOR_ROOT/lib/modules" || \
  die "Donor module tree is not platform complete"

python3 - "$LOADER_ARTIFACT" "$LOADER_MANIFEST" "$RECOVERY_ARTIFACT_DIR" "$LOADER_BUILD_SOURCE_RECORD" "$LOADER_SOURCE_REVISION_FILE" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys
image, manifest_path, recovery_dir, source_record_path, revision_path = map(Path, sys.argv[1:])
data = image.read_bytes()
if len(data) != 0x40000:
    raise SystemExit("meraki-redboot boot region must be exactly 256 KiB")
for marker in (b"PMOSRAM READY 2", b"PMOSBOOT MENU-PROBE", b"PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY"):
    if marker not in data:
        raise SystemExit(f"meraki-redboot boot region is missing {marker!r}")
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
cap = manifest.get("uart_ramloader", {})
if manifest.get("format") != "postmerkos.vcoreiii-linuxloader-build.v7":
    raise SystemExit("meraki-redboot manifest format is not v7")
if cap.get("enabled") is not True or cap.get("protocol_version") != 2:
    raise SystemExit("meraki-redboot manifest does not declare PMOSRAM v2")
if cap.get("boot_menu", {}).get("options") != {"1": "uart-ramloader", "2": "embedded-firmware-recovery"}:
    raise SystemExit("meraki-redboot manifest does not declare the v0.7 boot menu")
if cap.get("image_check_diagnostics") != "structured-pass-warn-fail-skip-values-v1":
    raise SystemExit("meraki-redboot manifest lacks structured image diagnostics")
policies = manifest.get("policies", {})
if policies.get("payload_slot_end") != 0x300000 or policies.get("hard_payload_limit") != 0x2bffe0:
    raise SystemExit("meraki-redboot payload policy does not match the postmerkOS layout")
if manifest.get("boot_region", {}).get("sha256") != hashlib.sha256(data).hexdigest():
    raise SystemExit("meraki-redboot manifest digest mismatch")
source_record = json.loads(source_record_path.read_text(encoding="utf-8"))
if source_record.get("project") != "Gadorach/meraki-redboot" or source_record.get("revision") != revision_path.read_text().strip():
    raise SystemExit("meraki-redboot source provenance does not match the selected checkout")
embedded = cap.get("embedded_recovery", {})
expected_targets = {
    "luton26": (1, "70000064", ["MS22", "MS22P", "MS220-8", "MS220-8P", "MS220-24", "MS220-24P"]),
    "jaguar1": (
        2, "70000068",
        [
            "MS320-24", "MS320-24P", "MS220-48", "MS220-48P", "MS220-48LP",
            "MS220-48FP", "MS320-48", "MS320-48P", "MS320-48LP", "MS320-48FP",
            "MS42", "MS42P",
        ],
    ),
}
for family, (fid, spi, expected_models) in expected_targets.items():
    payload = recovery_dir / f"recovery-{family}.bin"
    raw = payload.read_bytes()
    marker = f"PMOSRECOVERY3;SOC={family};FAMILY={fid};SPI={spi};PROTO=3;PREFLIGHT=4;BAUDTEST=1;FRAME_MAX=4096;WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;CONFIRM_RETRY=1;AUTO_CONFIRM=1;AUTO_REBOOT=1;END".encode()
    if raw.count(marker) != 1:
        raise SystemExit(f"{payload.name} target descriptor mismatch")
    descriptor = json.loads((recovery_dir / f"recovery-{family}.descriptor.json").read_text(encoding="utf-8"))
    binary = descriptor.get("binary", {})
    expected_geometry = {"bytes": 0x1000000, "erase_bytes": 0x10000, "page_bytes": 256, "address_bytes": 3}
    if descriptor.get("format") != "postmerkos.uart-recovery-payload.v3":
        raise SystemExit(f"{payload.name} descriptor format is unsupported")
    if descriptor.get("protocol_version") != 3 or descriptor.get("soc_family") != family:
        raise SystemExit(f"{payload.name} descriptor family/protocol mismatch")
    if descriptor.get("soc_family_id") != fid or descriptor.get("spi_software_mode_address") != int(spi, 16):
        raise SystemExit(f"{payload.name} descriptor target registers mismatch")
    if descriptor.get("accepted_flash_bytes") != 0x1000000 or descriptor.get("flash_geometry") != expected_geometry:
        raise SystemExit(f"{payload.name} descriptor flash geometry mismatch")
    if descriptor.get("operations") != ["verify", "preflight", "dry-run", "flash"]:
        raise SystemExit(f"{payload.name} descriptor operation contract mismatch")
    if descriptor.get("load_address") != 0x81000000 or descriptor.get("entry_address") != 0x81000000:
        raise SystemExit(f"{payload.name} descriptor load/entry address mismatch")
    if descriptor.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"{payload.name} descriptor lacks corrected byte-zero entry contract")
    if descriptor.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise SystemExit(f"{payload.name} descriptor lacks direct-member manifest lookup")
    if descriptor.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise SystemExit(f"{payload.name} descriptor lacks destructive SPI NOR preflight")
    if descriptor.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise SystemExit(f"{payload.name} descriptor lacks SPI master-enable correction")
    if descriptor.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"{payload.name} descriptor lacks adaptive PMOSREC v3 transport")
    scratch = descriptor.get("preflight_scratch", {})
    if scratch != {"default_address": 0x00FF0000, "bytes": 0x10000, "minimum_address": 0x40000, "restore_original": True}:
        raise SystemExit(f"{payload.name} descriptor preflight scratch contract mismatch")
    if descriptor.get("transport_integrity") != ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"]:
        raise SystemExit(f"{payload.name} descriptor integrity contract mismatch")
    jedec = descriptor.get("accepted_jedec_ids")
    if not isinstance(jedec, list) or not jedec or any(re.fullmatch(r"[0-9a-f]{6}", item or "") is None for item in jedec):
        raise SystemExit(f"{payload.name} descriptor JEDEC allow-list is invalid")
    if descriptor.get("accepted_models") != expected_models:
        raise SystemExit(f"{payload.name} descriptor model allow-list is invalid")
    if binary.get("filename") != payload.name or binary.get("bytes") != len(raw):
        raise SystemExit(f"{payload.name} descriptor binary record mismatch")
    digest = hashlib.sha256(raw).hexdigest()
    if str(binary.get("sha256", "")).lower() != digest:
        raise SystemExit(f"{payload.name} descriptor digest mismatch")
    embedded_record = embedded.get(family)
    if not isinstance(embedded_record, dict) or embedded_record.get("size") != len(raw) or str(embedded_record.get("sha256", "")).lower() != digest:
        raise SystemExit(f"{payload.name} does not match meraki-redboot embedded recovery metadata")
    if embedded_record.get("load_address") != 0x81000000 or embedded_record.get("entry_address") != 0x81000000:
        raise SystemExit(f"{payload.name} embedded recovery load/entry mismatch")
    if embedded_record.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"{payload.name} embedded recovery lacks corrected byte-zero entry contract")
    if embedded_record.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise SystemExit(f"{payload.name} embedded recovery lacks direct-member manifest lookup")
    if embedded_record.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise SystemExit(f"{payload.name} embedded recovery lacks hardware preflight support")
    if embedded_record.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise SystemExit(f"{payload.name} embedded recovery lacks SPI master-enable correction")
    if embedded_record.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"{payload.name} embedded recovery lacks adaptive PMOSREC v3 transport")
PY

entry="$(readelf -h "$KERNEL_ARTIFACT_DIR/vmlinuz" | awk '/Entry point address/ {print $4}')"
[[ "$entry" == 0x81000000 ]] || die "Unexpected compressed-kernel entry point: $entry"
(( $(file_size "$KERNEL_ARTIFACT_DIR/vmlinuz.bin") + 32 <= 0x2c0000 )) || \
  die "Compressed kernel and SPIM header exceed the kernel region"

manifest="$ARTIFACTS_DIR/build-inputs.sha256"
: > "$manifest"
record_tree() {
  local root="$1" file hash
  while IFS= read -r -d '' file; do
    hash="$(sha256sum "$file" | awk '{print $1}')"
    printf '%s  %s\n' "$hash" "${file#$REPO_ROOT/}" >> "$manifest"
  done < <(find "$root" -type f -print0 | sort -z)
}
for input in "$KERNEL_ARTIFACT_DIR/vmlinuz" "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" \
  "$KERNEL_HEADERS_TARBALL" "$LOADER_ARTIFACT" "$LOADER_MANIFEST" \
  "$LOADER_BUILD_SOURCE_RECORD" "$LOADER_SOURCE_REVISION_FILE" "$LOADER_SOURCE_VERSION_FILE" \
  "$ARTIFACTS_DIR/tools/mkvcoreiii_payload.py"; do
  hash="$(sha256sum "$input" | awk '{print $1}')"
  printf '%s  %s\n' "$hash" "${input#$REPO_ROOT/}" >> "$manifest"
done
record_tree "$RECOVERY_ARTIFACT_DIR"
record_tree "$DONOR_ROOT/lib/modules"
if bool_enabled "${INCLUDE_UI:-0}"; then record_tree "$BUILD_DIR/postmerkos-ui"; fi
for source_tree in \
  "$REPO_ROOT/scripts" \
  "$REPO_ROOT/buildroot/board/meraki/ms220" \
  "$REPO_ROOT/buildroot/packages" \
  "$REPO_ROOT/buildroot/patches"; do
  [[ -d "$source_tree" ]] && record_tree "$source_tree"
done
if bool_enabled "${INCLUDE_UI:-0}" && [[ -d "$REPO_ROOT/buildroot/features" ]]; then
  record_tree "$REPO_ROOT/buildroot/features"
fi
for source_file in "$REPO_ROOT/Makefile"; do
  hash="$(sha256sum "$source_file" | awk '{print $1}')"
  printf '%s  %s\n' "$hash" "${source_file#$REPO_ROOT/}" >> "$manifest"
done
sort -u -o "$manifest" "$manifest"
log "Verified local build inputs; manifest written to $manifest"
