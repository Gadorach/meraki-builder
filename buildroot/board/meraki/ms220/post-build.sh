#!/bin/sh
set -eu

: "${TARGET_DIR:?Buildroot TARGET_DIR is not set}"

BOARD_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
VENDOR_MODULES="$BOARD_DIR/vendor-modules"
[ -d "$VENDOR_MODULES" ] || {
    echo "Vendor module staging tree is missing: $VENDOR_MODULES" >&2
    exit 1
}

# Install the complete binary switch-module tree during post-build as the
# authoritative path. The image is intentionally platform agnostic: it carries
# Luton26, single-chip Jaguar1, dual-chip Jaguar1, all common objects, and any
# additional donor .ko files. S08kmods selects one family after board detection.
MODULE_TOOL="$BOARD_DIR/vendor-module-tree.py"
REQUIRED_MODULES="$BOARD_DIR/vendor-modules.required"
[ -x "$MODULE_TOOL" ] && [ -s "$REQUIRED_MODULES" ] || {
    echo "Vendor module verification tooling is missing from the board tree" >&2
    exit 1
}
python3 "$MODULE_TOOL" verify "$VENDOR_MODULES" \
    --required-file "$REQUIRED_MODULES" --quiet || {
    echo "Board-local vendor module tree is incomplete" >&2
    exit 1
}
rm -rf "$TARGET_DIR/lib/modules"
mkdir -p "$TARGET_DIR/lib/modules"
cp -a "$VENDOR_MODULES/." "$TARGET_DIR/lib/modules/"
python3 "$MODULE_TOOL" verify "$TARGET_DIR/lib/modules" \
    --required-file "$REQUIRED_MODULES" --quiet || {
    echo "Installed rootfs vendor module tree is incomplete" >&2
    exit 1
}

mkdir -p "$TARGET_DIR/overlay" "$TARGET_DIR/click" "$TARGET_DIR/etc" "$TARGET_DIR/usr/share/postmerkos"
ln -snf /overlay "$TARGET_DIR/config"

# Dropbear host keys are created in the persistent JFFS2-backed /etc overlay.
if [ -L "$TARGET_DIR/etc/dropbear" ]; then
    rm -f "$TARGET_DIR/etc/dropbear"
fi
mkdir -p "$TARGET_DIR/etc/dropbear"

if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    build_version="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%d-%H-%M 2>/dev/null || date -u +%Y-%m-%d-%H-%M)"
else
    build_version="$(date -u +%Y-%m-%d-%H-%M)"
fi
release="${POSTMERKOS_RELEASE:-${MS42P_RELEASE:-$build_version}}"
revision="${POSTMERKOS_GIT_REVISION:-unknown}"
# Compatibility is release-specific. Every supported exact model is present,
# while only models explicitly validated for this build are promoted.
validated_models="${POSTMERKOS_VALIDATED_MODELS:-MS42P MS320-24P}"
loader_manifest="${MS42P_LOADER_MANIFEST:-}"
recovery_artifact_dir="${MS42P_RECOVERY_ARTIFACT_DIR:-}"
[ -s "$loader_manifest" ] || { echo "meraki-redboot capability manifest is missing" >&2; exit 1; }
[ -d "$recovery_artifact_dir" ] || { echo "Recovery artifact directory is missing" >&2; exit 1; }
python3 - "$TARGET_DIR/etc/postmerkos-release.json" "$release" "$build_version" "$revision" "$validated_models" "$loader_manifest" "$recovery_artifact_dir" <<'PY_RELEASE'
import hashlib
import json
import re
import sys
from pathlib import Path

output, release, build_time, revision, validated_raw, loader_manifest_path, recovery_dir_raw = sys.argv[1:]
recovery_dir = Path(recovery_dir_raw)
models = [
    "MS22", "MS22P",
    "MS220-8", "MS220-8P", "MS220-24", "MS220-24P",
    "MS220-48", "MS220-48P", "MS220-48LP", "MS220-48FP",
    "MS320-24", "MS320-24P", "MS320-48", "MS320-48P",
    "MS320-48LP", "MS320-48FP",
    "MS42", "MS42P",
]
validated = {item.strip() for item in validated_raw.replace(",", " ").split() if item.strip()}
unknown = sorted(validated.difference(models))
if unknown:
    raise SystemExit("POSTMERKOS_VALIDATED_MODELS contains unsupported exact model(s): " + ", ".join(unknown))
loader_manifest = json.loads(Path(loader_manifest_path).read_text(encoding="utf-8"))
uart = loader_manifest.get("uart_ramloader", {})
if uart.get("enabled") is not True or uart.get("protocol_version") != 2:
    raise SystemExit("meraki-redboot manifest does not declare UART RAM-loader v2")
if loader_manifest.get("format") != "postmerkos.vcoreiii-linuxloader-build.v7":
    raise SystemExit("meraki-redboot manifest format is not v7")
if uart.get("boot_menu", {}).get("options") != {"1": "uart-ramloader", "2": "embedded-firmware-recovery"}:
    raise SystemExit("meraki-redboot manifest does not declare the v0.7 boot menu")
if uart.get("image_check_diagnostics") != "structured-pass-warn-fail-skip-values-v1":
    raise SystemExit("meraki-redboot manifest lacks structured diagnostics")
if loader_manifest.get("policies", {}).get("payload_slot_end") != 0x300000:
    raise SystemExit("meraki-redboot payload slot boundary does not match postmerkOS")
embedded = uart.get("embedded_recovery", {})
payloads = {}
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
expected_geometry = {"bytes": 16 * 1024 * 1024, "erase_bytes": 64 * 1024, "page_bytes": 256, "address_bytes": 3}
for family in ("luton26", "jaguar1"):
    descriptor_path = recovery_dir / f"recovery-{family}.descriptor.json"
    if not descriptor_path.is_file():
        raise SystemExit(f"Recovery descriptor is missing: {descriptor_path}")
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    binary = descriptor.get("binary", {})
    binary_path = recovery_dir / str(binary.get("filename", ""))
    if descriptor.get("format") != "postmerkos.uart-recovery-payload.v3":
        raise SystemExit(f"Invalid recovery descriptor format: {descriptor_path}")
    if descriptor.get("protocol_version") != 3 or descriptor.get("soc_family") != family:
        raise SystemExit(f"Recovery descriptor family/protocol mismatch: {descriptor_path}")
    if descriptor.get("soc_family_id") != expected[family]["id"] or descriptor.get("spi_software_mode_address") != expected[family]["spi"]:
        raise SystemExit(f"Recovery descriptor target register mismatch: {descriptor_path}")
    geometry = descriptor.get("flash_geometry")
    jedec = descriptor.get("accepted_jedec_ids")
    accepted_models = descriptor.get("accepted_models")
    if descriptor.get("accepted_flash_bytes") != 16 * 1024 * 1024 or geometry != expected_geometry:
        raise SystemExit(f"Recovery descriptor flash geometry mismatch: {descriptor_path}")
    if descriptor.get("operations") != ["verify", "preflight", "dry-run", "flash"]:
        raise SystemExit(f"Recovery descriptor operation contract mismatch: {descriptor_path}")
    if descriptor.get("load_address") != 0x81000000 or descriptor.get("entry_address") != 0x81000000:
        raise SystemExit(f"Recovery descriptor load/entry address mismatch: {descriptor_path}")
    if descriptor.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"Recovery descriptor lacks corrected byte-zero entry contract: {descriptor_path}")
    if descriptor.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise SystemExit(f"Recovery descriptor lacks direct-member manifest lookup: {descriptor_path}")
    if descriptor.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise SystemExit(f"Recovery descriptor lacks destructive SPI NOR preflight: {descriptor_path}")
    if descriptor.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise SystemExit(f"Recovery descriptor lacks SPI master-enable correction: {descriptor_path}")
    if descriptor.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"Recovery descriptor lacks PMOSREC v3 adaptive transport: {descriptor_path}")
    scratch = descriptor.get("preflight_scratch", {})
    if scratch != {"default_address": 0x00FF0000, "bytes": 0x10000, "minimum_address": 0x40000, "restore_original": True}:
        raise SystemExit(f"Recovery descriptor scratch-sector contract mismatch: {descriptor_path}")
    if descriptor.get("transport_integrity") != ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"]:
        raise SystemExit(f"Recovery descriptor integrity contract mismatch: {descriptor_path}")
    if not isinstance(jedec, list) or not jedec or any(not isinstance(item, str) or re.fullmatch(r"[0-9a-f]{6}", item) is None for item in jedec):
        raise SystemExit(f"Recovery descriptor JEDEC allow-list is invalid: {descriptor_path}")
    if accepted_models != expected[family]["models"] or any(model not in models for model in accepted_models):
        raise SystemExit(f"Recovery descriptor model allow-list is invalid: {descriptor_path}")
    if not binary_path.is_file() or binary_path.stat().st_size != binary.get("bytes"):
        raise SystemExit(f"Recovery payload size does not match descriptor: {binary_path}")
    payload_data = binary_path.read_bytes()
    marker = (
        f"PMOSRECOVERY3;SOC={family};FAMILY={expected[family]['id']};"
        f"SPI={expected[family]['spi']:08x};PROTO=3;PREFLIGHT=4;BAUDTEST=1;"
        "FRAME_MAX=4096;WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;"
        "CONFIRM_RETRY=1;AUTO_CONFIRM=1;AUTO_REBOOT=1;END"
    ).encode("ascii")
    if payload_data.count(marker) != 1:
        raise SystemExit(f"Recovery payload embedded target descriptor mismatch: {binary_path}")
    digest = hashlib.sha256(payload_data).hexdigest()
    if digest != str(binary.get("sha256", "")).lower():
        raise SystemExit(f"Recovery payload digest does not match descriptor: {binary_path}")
    embedded_record = embedded.get(family)
    if not isinstance(embedded_record, dict) or embedded_record.get("size") != len(payload_data) or str(embedded_record.get("sha256", "")).lower() != digest:
        raise SystemExit(f"meraki-redboot embedded recovery binding mismatch: {family}")
    if embedded_record.get("load_address") != 0x81000000 or embedded_record.get("entry_address") != 0x81000000:
        raise SystemExit(f"meraki-redboot embedded recovery load/entry mismatch: {family}")
    if embedded_record.get("entry_contract") != "flat-binary-byte-zero-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks corrected byte-zero entry contract: {family}")
    if embedded_record.get("manifest_lookup_contract") != "direct-object-members-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks direct-member manifest lookup: {family}")
    if embedded_record.get("hardware_preflight_contract") != "spi-nor-scratch-rw-restore-loader-crc-v4":
        raise SystemExit(f"meraki-redboot embedded recovery lacks hardware preflight: {family}")
    if embedded_record.get("spi_master_enable_contract") != "preserve-general-ctrl-enable-spi-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks SPI master-enable correction: {family}")
    if embedded_record.get("adaptive_transport_contract") != "pmosrec-v3-adaptive-uart-sparse-lz4-v1":
        raise SystemExit(f"meraki-redboot embedded recovery lacks adaptive PMOSREC v3 transport: {family}")
    if common_geometry is None:
        common_geometry, common_jedec = geometry, jedec
    elif common_geometry != geometry or common_jedec != jedec:
        raise SystemExit("Recovery descriptors disagree on flash constraints")
    payloads[family] = {
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
data = {
    "version": release,
    "build_time_utc": build_time,
    "git_revision": revision,
    "target_family": "vcore3",
    "image_format": 2,
    "config_schema": 3,
    "fwupdate_api": 2,
    "web_api": 2,
    "models": {
        model: ("validated" if model in validated else "untested")
        for model in models
    },
    "recovery": {
        "uart_ramloader": {
            "enabled": True,
            "protocol_version": 2,
            "probe_timeout_ms": int(uart["probe_timeout_ms"]),
            "interbyte_timeout_ms": int(uart["interbyte_timeout_ms"]),
            "maximum_payload_bytes": int(uart["maximum_payload_bytes"]),
            "ram_start": int(uart["ram_start"]),
            "ram_end": int(uart["ram_end"]),
            "supported_soc_families": list(uart["supported_soc_families"]),
            "transport_integrity": list(uart["transport_integrity"]),
            "loader_sha256": str(loader_manifest["boot_region"]["sha256"]),
            "embedded_recovery": {
                family: {
                    "bytes": payloads[family]["bytes"],
                    "sha256": payloads[family]["sha256"],
                    "load_address": payloads[family]["load_address"],
                    "entry_address": payloads[family]["entry_address"],
                    "entry_contract": payloads[family]["entry_contract"],
                    "manifest_lookup_contract": payloads[family]["manifest_lookup_contract"],
                    "hardware_preflight_contract": payloads[family]["hardware_preflight_contract"],
                    "spi_master_enable_contract": payloads[family]["spi_master_enable_contract"],
                    "adaptive_transport_contract": payloads[family]["adaptive_transport_contract"],
                } for family in ("luton26", "jaguar1")
            },
        },
        "uart_firmware": {
            "enabled": True,
            "protocol_version": 3,
            "full_image_bytes": 16 * 1024 * 1024,
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
            "preflight_scratch": {"default_address": 0x00FF0000, "bytes": 0x10000, "minimum_address": 0x40000, "restore_original": True},
            "transport_integrity": ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"],
            "flash_geometry": common_geometry,
            "accepted_jedec_ids": common_jedec,
            "payloads": payloads,
        },
    },
}
Path(output).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PY_RELEASE

cat > "$TARGET_DIR/etc/lsb-release" <<EOF_RELEASE
DISTRIB_ID="postmerkOS"
DISTRIB_RELEASE="$release"
DISTRIB_DESCRIPTION="postmerkOS VCore-III mipsel"
EOF_RELEASE

# S14passwd contains a source-controlled placeholder. Replace it with the salt
# generated by Buildroot for the root account without relying on an unsafe sed
# replacement string.
passwd_script="$TARGET_DIR/etc/init.d/S14passwd"
if [ -f "$passwd_script" ] && grep -qF '__SALT__' "$passwd_script"; then
    salt="$(awk -F: '$1 == "root" { split($2, p, "\\$"); print p[3]; exit }' "$TARGET_DIR/etc/shadow")"
    [ -n "$salt" ] || { echo "Unable to determine root password salt" >&2; exit 1; }
    python3 - "$passwd_script" "$salt" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
path.write_text(path.read_text().replace('__SALT__', sys.argv[2]))
PY
fi

find "$TARGET_DIR/etc/init.d" -maxdepth 1 -type f -exec chmod 0755 {} +
