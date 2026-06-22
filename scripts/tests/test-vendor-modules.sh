#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BOARD_DIR="$REPO_ROOT/buildroot/board/meraki/ms220"
MODULE_TOOL="$BOARD_DIR/vendor-module-tree.py"
REQUIRED_FILE="$BOARD_DIR/vendor-modules.required"
tmp_base=${POSTMERKOS_TEST_TMPDIR:-${TMPDIR:-/tmp}}
TMP=$(mktemp -d "$tmp_base/postmerkos-modules.XXXXXX")
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

WORK="$TMP/work"
DONOR="$WORK/extracted/donor-rootfs"
MODULES="$DONOR/lib/modules"
mkdir -p "$MODULES" "$DONOR/usr/lib/vendor"/{luton26,jaguar,jaguar_dual,extra}
printf 'elts\n' > "$MODULES/elts_meraki.ko"
printf 'click\n' > "$MODULES/merakiclick.ko"
printf 'proc\n' > "$MODULES/proclikefs.ko"

# Exercise absolute and relative links in all supported platform families.
for family in luton26 jaguar jaguar_dual; do
  mkdir -p "$MODULES/$family"
  printf '%s-vc-click\n' "$family" > "$DONOR/usr/lib/vendor/$family/vc_click.ko"
  printf '%s-vtss-core\n' "$family" > "$DONOR/usr/lib/vendor/$family/vtss_core.ko"
  ln -s "/usr/lib/vendor/$family/vc_click.ko" "$MODULES/$family/vc_click.ko"
  ln -s "../../../usr/lib/vendor/$family/vtss_core.ko" "$MODULES/$family/vtss_core.ko"
done

# Unknown donor modules must also survive. The image carries the entire donor
# module inventory, while the required contract guarantees the known families.
mkdir -p "$MODULES/vendor_helpers"
printf 'extra-donor-module\n' > "$DONOR/usr/lib/vendor/extra/vendor_diag.ko"
ln -s ../../../usr/lib/vendor/extra/vendor_diag.ko "$MODULES/vendor_helpers/vendor_diag.ko"

NORMALIZED="$TMP/materialized"
python3 "$SCRIPT_DIR/materialize-module-tree.py" \
  --donor-root "$DONOR" --source "$MODULES" --output "$NORMALIZED"
rm -rf "$MODULES"
mv "$NORMALIZED" "$MODULES"
python3 "$MODULE_TOOL" create "$MODULES" --required-file "$REQUIRED_FILE" --quiet
python3 "$MODULE_TOOL" verify "$MODULES" --required-file "$REQUIRED_FILE" --quiet

for module in \
    elts_meraki.ko merakiclick.ko proclikefs.ko \
    luton26/vc_click.ko luton26/vtss_core.ko \
    jaguar/vc_click.ko jaguar/vtss_core.ko \
    jaguar_dual/vc_click.ko jaguar_dual/vtss_core.ko \
    vendor_helpers/vendor_diag.ko; do
  [ -s "$MODULES/$module" ]
  [ ! -L "$MODULES/$module" ]
done

DEST1="$TMP/generated-overlay/lib/modules"
DEST2="$TMP/prepared-buildroot/board/meraki/ms220/vendor-modules"
REPO_ROOT="$REPO_ROOT" MS42P_WORK_DIR="$WORK" \
  MS42P_ARTIFACTS_DIR="$TMP/artifacts" \
  "$SCRIPT_DIR/stage-vendor-modules.sh" "$DEST1" "$DEST2"
for destination in "$DEST1" "$DEST2"; do
  python3 "$MODULE_TOOL" verify "$destination" --required-file "$REQUIRED_FILE" --quiet
  [ -s "$destination/vendor_helpers/vendor_diag.ko" ]
done

# Exercise the authoritative post-build install path in isolation.
BOARD="$TMP/board/meraki/ms220"
mkdir -p "$BOARD" "$TMP/target/etc/init.d"
cp "$REPO_ROOT/buildroot/board/meraki/ms220/post-build.sh" "$BOARD/post-build.sh"
cp "$MODULE_TOOL" "$BOARD/vendor-module-tree.py"
cp "$REQUIRED_FILE" "$BOARD/vendor-modules.required"
cp -a "$DEST2" "$BOARD/vendor-modules"

# Supply the source-owned loader and recovery capability records required by
# the authoritative post-build release manifest path.
RECOVERY="$TMP/recovery"
LOADER_MANIFEST="$TMP/loader.manifest.json"
mkdir -p "$RECOVERY"
python3 - "$LOADER_MANIFEST" "$RECOVERY" <<'PY_RECOVERY_FIXTURE'
import hashlib
import json
from pathlib import Path
import sys
loader_manifest, recovery_raw = sys.argv[1:]
recovery = Path(recovery_raw)
targets = {
    "luton26": {
        "id": 1, "spi": 0x70000064,
        "models": ["MS22", "MS22P", "MS220-8", "MS220-8P", "MS220-24", "MS220-24P"],
    },
    "jaguar1": {
        "id": 2, "spi": 0x70000068,
        "models": [
            "MS320-24", "MS320-24P", "MS220-48", "MS220-48P", "MS220-48LP",
            "MS220-48FP", "MS320-48", "MS320-48P", "MS320-48LP", "MS320-48FP",
            "MS42", "MS42P",
        ],
    },
}
geometry = {"bytes": 0x1000000, "erase_bytes": 0x10000, "page_bytes": 256, "address_bytes": 3}
embedded = {}
jedec = ["c22018", "ef4018", "012018", "20ba18", "c84018"]
for family, target in targets.items():
    marker = (
        f"PMOSRECOVERY3;SOC={family};FAMILY={target['id']};"
        f"SPI={target['spi']:08x};PROTO=3;PREFLIGHT=4;BAUDTEST=1;FRAME_MAX=4096;"
        "WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;CONFIRM_RETRY=1;"
        "AUTO_CONFIRM=1;AUTO_REBOOT=1;END"
    ).encode()
    payload = recovery / f"recovery-{family}.bin"
    payload.write_bytes(b"module-test-payload\0" + marker + b"\0")
    raw = payload.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    embedded[family] = {
        "path": str(payload), "size": len(raw), "sha256": digest,
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
        "accepted_flash_bytes": 0x1000000,
        "accepted_jedec_ids": jedec,
        "flash_geometry": geometry,
        "operations": ["verify", "preflight", "dry-run", "flash"],
        "transport_integrity": ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"],
        "adaptive_transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
        "load_address": 0x81000000,
        "entry_address": 0x81000000,
        "entry_contract": "flat-binary-byte-zero-v1",
        "manifest_lookup_contract": "direct-object-members-v1",
        "hardware_preflight_contract": "spi-nor-scratch-rw-restore-loader-crc-v4",
        "spi_master_enable_contract": "preserve-general-ctrl-enable-spi-v1",
        "preflight_scratch": {"default_address": 0x00FF0000, "bytes": 0x10000, "minimum_address": 0x40000, "restore_original": True},
        "binary": {
            "filename": payload.name,
            "bytes": len(raw),
            "sha256": digest,
        },
    }
    (recovery / f"recovery-{family}.descriptor.json").write_text(
        json.dumps(descriptor, indent=2, sort_keys=True) + "\n"
    )
Path(loader_manifest).write_text(json.dumps({
    "format": "postmerkos.vcoreiii-linuxloader-build.v7",
    "variant": "development",
    "boot_region": {"size": 0x40000, "sha256": "00" * 32},
    "policies": {
        "crc": "warn", "size": "legacy-warn",
        "payload_slot_end": 0x300000, "hard_payload_limit": 0x2BFFE0,
    },
    "toolchain": {"id": "module-test-gcc473"},
    "uart_ramloader": {
        "enabled": True,
        "protocol_version": 2,
        "probe_timeout_ms": 3000,
        "interbyte_timeout_ms": 3000,
        "menu_selection_timeout_ms": 5000,
        "maximum_payload_bytes": 4 * 1024 * 1024,
        "ram_start": 0x81000000,
        "ram_end": 0x87f00000,
        "supported_soc_families": ["luton26", "jaguar1"],
        "transport_integrity": ["frame-crc32", "compact-ack-crc32", "object-crc32", "object-sha256", "reconstructed-image-sha256"],
        "adaptive_transport_contract": "pmosrec-v3-adaptive-uart-sparse-lz4-v1",
        "boot_menu": {
            "probe_timeout_ms": 3000,
            "selection_timeout_ms": 5000,
            "options": {"1": "uart-ramloader", "2": "embedded-firmware-recovery"},
            "noise_behavior": "invalid/no explicit option continues normal boot",
        },
        "image_check_diagnostics": "structured-pass-warn-fail-skip-values-v1",
        "embedded_recovery": embedded,
    },
}, indent=2, sort_keys=True) + "\n")
PY_RECOVERY_FIXTURE
TARGET_DIR="$TMP/target" POSTMERKOS_RELEASE=test \
  MS42P_LOADER_MANIFEST="$LOADER_MANIFEST" \
  MS42P_RECOVERY_ARTIFACT_DIR="$RECOVERY" \
  "$BOARD/post-build.sh"
python3 "$MODULE_TOOL" verify "$TMP/target/lib/modules" \
  --required-file "$REQUIRED_FILE" --quiet
[ -s "$TMP/target/lib/modules/vendor_helpers/vendor_diag.ko" ]

# A donor missing any supported family must never be accepted as reusable or
# staged into a supposedly platform-agnostic image.
BROKEN="$TMP/broken"
cp -a "$MODULES" "$BROKEN"
rm -f "$BROKEN/jaguar/vc_click.ko"
if python3 "$MODULE_TOOL" create "$BROKEN" --required-file "$REQUIRED_FILE" --quiet; then
  echo 'incomplete Jaguar family unexpectedly passed module contract creation' >&2
  exit 1
fi

# Removing an extra donor module without regenerating metadata must also fail,
# proving that all donor .ko files—not only the required matrix—are tracked.
BROKEN_EXTRA="$TMP/broken-extra"
cp -a "$MODULES" "$BROKEN_EXTRA"
rm -f "$BROKEN_EXTRA/vendor_helpers/vendor_diag.ko"
if python3 "$MODULE_TOOL" verify "$BROKEN_EXTRA" --required-file "$REQUIRED_FILE" --quiet; then
  echo 'lost extra donor module unexpectedly passed complete-inventory verification' >&2
  exit 1
fi

# Confirm that one image self-identifies each supported ASIC family and selects
# only that family's pair at boot. The profile action does not load modules.
KMOD_INIT="$REPO_ROOT/buildroot/board/meraki/ms220/overlay/etc/init.d/S08kmods"
check_profile() {
  model=$1 expected_family=$2 expected_board=$3 expected_ports=$4
  boardinfo="$TMP/boardinfo"
  ports="$TMP/NUM_PORTS"
  printf 'MODEL=%s\n' "$model" > "$boardinfo"
  output=$(POSTMERKOS_BOARDINFO="$boardinfo" \
    POSTMERKOS_CPUINFO="$TMP/no-cpuinfo" \
    POSTMERKOS_NUM_PORTS_FILE="$ports" \
    POSTMERKOS_BOARD_PROFILE="$REPO_ROOT/buildroot/board/meraki/ms220/overlay/usr/sbin/postmerkos-board-profile" \
    "$KMOD_INIT" profile)
  printf '%s\n' "$output" | grep -q "^MODULE_FAMILY=$expected_family$"
  printf '%s\n' "$output" | grep -q "^BOARD_DESCRIPTOR=$expected_board$"
  printf '%s\n' "$output" | grep -q "^LOGICAL_PORTS=$expected_ports$"
  [ "$(cat "$ports")" = "$expected_ports" ]
}
check_profile MS220-8P luton26 MERAKI_BOARD_MS220_8 10
check_profile MS220-24P luton26 MERAKI_BOARD_MS220_24 26
check_profile MS220-48LP jaguar_dual MERAKI_BOARD_MS220_48 52
check_profile MS320-24P jaguar MERAKI_BOARD_MS320_24 28
check_profile MS320-48FP jaguar_dual MERAKI_BOARD_MS320_48 52
check_profile MS22P luton26 MERAKI_BOARD_MS22 26
check_profile MS42P jaguar_dual MERAKI_BOARD_MS42 52
printf 'MODEL=unsupported\n' > "$TMP/boardinfo"
if POSTMERKOS_BOARDINFO="$TMP/boardinfo" \
   POSTMERKOS_CPUINFO="$TMP/no-cpuinfo" \
   POSTMERKOS_NUM_PORTS_FILE="$TMP/NUM_PORTS" \
   POSTMERKOS_BOARD_PROFILE="$REPO_ROOT/buildroot/board/meraki/ms220/overlay/usr/sbin/postmerkos-board-profile" \
   "$KMOD_INIT" profile >/dev/null 2>&1; then
  echo 'unsupported platform unexpectedly produced a module profile' >&2
  exit 1
fi

echo 'platform-complete module inclusion and boot-time family-selection tests passed'
