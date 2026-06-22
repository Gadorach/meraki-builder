#!/usr/bin/env bash
set -Eeuo pipefail
shopt -s nullglob

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
ARTIFACTS_DIR=${ARTIFACTS_DIR:-"$REPO_ROOT/artifacts"}
TFTP_PORT=${TFTP_PORT:-1069}
CONFIG_DIR=${XDG_CONFIG_HOME:-"$HOME/.config"}/meraki-fw-flasher
KNOWN_HOSTS="$CONFIG_DIR/known_hosts"
LOG_DIR=${MERAKI_FLASH_LOG_DIR:-"$REPO_ROOT/logs"}
MODE=modern
FLASH_SCOPE=system
FLASH_SCOPE_REQUESTED=0
FIRMWARE_VERSION=""
CONTROL_PATH=""
SELECTED_FIRMWARE=""
TMP=""
TFTP_PID=""
SSH_PASSWORD=""
SSH_AUTH_MODE=key
ALLOW_UNTESTED=0
TRANSPORT=tftp
BOOTLOADER_RECOVERY=0
BOOTLOADER_PAYLOAD=${BOOTLOADER_PAYLOAD:-}
BOOTLOADER_RECOVERY_PATH=${BOOTLOADER_RECOVERY_PATH:-ram-upload}
SERIAL_DEVICE=${SERIAL_DEVICE:-}
TARGET_MODEL=${TARGET_MODEL:-}
FORCE_FLASH=0
OPERATION=""
OPERATION_PRESELECTED=0
PREFLIGHT_SCRATCH=${PREFLIGHT_SCRATCH:-0x00ff0000}
SUPPRESS_ERR_REPORT=0
MANUAL_TARGET_CONFIRMATION=0
VERBOSE_ACKS=0
SKIP_BAUD_NEGOTIATION=0
DIAGNOSTIC_BAUD_SCAN=0
DIAGNOSTIC_WINDOW_SCAN=0

usage() {
    cat <<'USAGE'
usage: firmware-flasher.sh [options]

Select a postmerkOS firmware artifact, publish it from a temporary read-only
TFTP server, and start the switch updater through SSH or hardware serial.

The current manifest-aware firmware workflow is the default. Checksum-only mode
uses the current updater with only the original image and .sha256 artifacts. Use
--legacy only for older firmware that lacks the management CLI and manifest flags.

Options:
  --artifacts DIR       artifact directory (default: ../../artifacts)
  --firmware FILE       preselect an artifact instead of opening the list
  --tftp-port PORT      unprivileged TFTP port (default: 1069)
  --control METHOD      ssh, serial, or bootloader; otherwise prompt
  --transport METHOD    tftp (default) or uart; UART supports serial or bootloader control
  --bootloader-recovery use meraki-redboot pre-kernel UART recovery
  --bootloader-preflight run UART/SPI/NOR erase-program-readback-restore test
  --recovery-path PATH ram-upload (default), embedded, or auto
  --recovery-payload FILE external payload for ram-upload/legacy fallback
  --target-model MODEL  exact hardware model required for bootloader recovery
  --preflight-scratch N  aligned 64 KiB NOR address (default: 0x00ff0000)
  --manual-target-confirmation  require manual ERASEFLASH challenge entry
  --verbose-acks        print every decoded compact ACK (always retained in logs)
  --skip-baud-negotiation keep PMOSREC at 115200 for diagnostics
  --diagnostic-baud-scan use the legacy broad rate scan and midpoint refinement
  --diagnostic-window-scan test paced multi-frame windows after safe window 1
  --serial-device DEV   serial character device
  --modern              current manifest-aware workflow (default)
  --checksum-only       current updater with image + .sha256 only
  --legacy              older direct fw_update_tftp workflow
  --full-flash          overwrite bootloader, kernel, SquashFS, and JFFS2
  --system-flash        normal SquashFS/JFFS2 update scope (default)
  --version VERSION     version hint for checksum-only artifacts
  --self-test           test the private TFTP server and bundle validation
  --help                show this help

Modern artifact set:
  image.bin
  image.bin.sha256
  image.bin.manifest.json
  image.bin.manifest.json.sha256

Checksum-only artifact set:
  image.bin
  image.bin.sha256

Full flash accepts only an exact 16 MiB image and is unavailable in legacy mode.

Bootloader recovery requires a source-built meraki-redboot with PMOSRAM protocol v2.
The host verifies the exact model, SoC-specific recovery payload, release manifest,
16 MiB SPIM/SquashFS layout, loader capability, SHA-256 digests, and frame CRCs.
Verify performs local checks only; dry-run transfers and validates without erasing.

UART transport converts the binary into PMOSUART/1 Base64 frames with a CRC-32
per frame and whole-object SHA-256 validation. It uses target RAM under /run.

Modern serial control explicitly handles the current postmerkOS getty and
management menu. It asks the getty/menu for a permitted raw shell, then runs
`postmerkos-console firmware tftp ...`. It does not assume login opens a shell.
USAGE
}

log() { printf '[flasher] %s\n' "$*"; }
warn() { printf '[flasher] warning: %s\n' "$*" >&2; }
die() { printf '[flasher] error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required command is missing: $1"; }

report_unexpected_error() {
    local rc=$? line=${BASH_LINENO[0]:-unknown} command=${BASH_COMMAND:-unknown}
    (( SUPPRESS_ERR_REPORT )) && return "$rc"
    # Expected probe failures used by if/while/|| are excluded from ERR by Bash.
    # Anything reaching this trap is therefore an unexpected set -e termination.
    printf '[flasher] error: unexpected command failure (status %s) at line %s: %s\n' \
        "$rc" "$line" "$command" >&2
    return "$rc"
}
trap report_unexpected_error ERR

stop_tftp_server() {
    local attempt
    [[ -n ${TFTP_PID:-} ]] || return 0
    kill "$TFTP_PID" 2>/dev/null || true
    for attempt in {1..20}; do
        kill -0 "$TFTP_PID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$TFTP_PID" 2>/dev/null; then
        warn "TFTP server did not stop promptly; forcing termination"
        kill -KILL "$TFTP_PID" 2>/dev/null || true
    fi
    wait "$TFTP_PID" 2>/dev/null || true
    TFTP_PID=""
}

cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    stop_tftp_server
    SSH_PASSWORD=""
    [[ -n $TMP && -d $TMP ]] && rm -rf -- "$TMP"
    exit "$rc"
}
trap cleanup EXIT INT TERM

prompt_default() {
    local prompt=$1 default=$2 value
    read -r -p "$prompt [$default]: " value
    printf '%s' "${value:-$default}"
}

prompt_yes_no() {
    local prompt=$1 default=${2:-y} answer suffix
    [[ $default == y ]] && suffix='[Y/n]' || suffix='[y/N]'
    read -r -p "$prompt $suffix " answer
    answer=${answer:-$default}
    [[ $answer =~ ^[Yy]([Ee][Ss])?$ ]]
}

read_magic_hex() {
    dd if="$1" bs=1 skip="$2" count=4 status=none 2>/dev/null |
        od -An -tx1 | tr -d ' \n'
}

classify_firmware() {
    local file=$1 size magic0 kernel_magic rootfs_magic
    size=$(stat -c %s -- "$file" 2>/dev/null) || return 1
    magic0=$(read_magic_hex "$file" 0)
    if (( size == 16777216 )); then
        kernel_magic=$(read_magic_hex "$file" $((0x40000)))
        rootfs_magic=$(read_magic_hex "$file" $((0x300000)))
        [[ $rootfs_magic == 68737173 ]] || return 1
        if [[ $kernel_magic == 5350494d ]]; then
            printf 'full'
        else
            printf 'raw-full'
        fi
        return
    fi
    if (( size > 0 && size <= 8388608 )) && [[ $magic0 == 68737173 ]]; then
        printf 'squashfs'
        return
    fi
    return 1
}

human_size() {
    awk -v b="$1" 'BEGIN {
        if (b >= 1073741824) printf "%.2f GiB", b/1073741824;
        else if (b >= 1048576) printf "%.2f MiB", b/1048576;
        else if (b >= 1024) printf "%.2f KiB", b/1024;
        else printf "%d B", b;
    }'
}

sidecar_values() {
    local sidecar=$1
    awk 'NF >= 2 && $1 !~ /^#/ {
        hash=tolower($1); $1=""; sub(/^[ \t]+[*]?/, ""); sub(/\r$/, "");
        print hash "|" $0; exit
    }' "$sidecar"
}

verify_sidecar() {
    local file=$1 sidecar=$2 values expected listed actual
    [[ -f $sidecar ]] || return 1
    values=$(sidecar_values "$sidecar") || return 1
    IFS='|' read -r expected listed <<< "$values"
    [[ $expected =~ ^[0-9a-f]{64}$ ]] || return 1
    [[ $listed == "$(basename -- "$file")" ]] || return 1
    actual=$(sha256sum "$file" | awk '{print $1}')
    [[ $actual == "$expected" ]]
}

validate_checksum_bundle() {
    local image=$1
    verify_sidecar "$image" "$image.sha256"
}

detect_embedded_version() {
    python3 - "$1" "$2" <<'PY_VERSION'
import json
import os
import sys

path, image_type = sys.argv[1:]
slot = 4096
root_size = 8 * 1024 * 1024
root_offset = 3 * 1024 * 1024 if image_type in {"full", "raw-full"} else 0
size = os.path.getsize(path)
if size < root_offset + slot:
    raise SystemExit(1)
with open(path, 'rb') as stream:
    stream.seek(root_offset + root_size - slot)
    block = stream.read(slot)
if len(block) < 16 or block[:8] != b'PMOSMETA':
    raise SystemExit(1)
try:
    length = int(block[8:16], 16)
    payload = json.loads(block[16:16 + length].decode('utf-8'))
except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
    raise SystemExit(1)
version = payload.get('version')
if not isinstance(version, str) or not version or len(version) > 127:
    raise SystemExit(1)
print(version)
PY_VERSION
}

validate_manifest_bundle() {
    local image=$1 manifest="$1.manifest.json" manifest_sha="$1.manifest.json.sha256"
    verify_sidecar "$image" "$image.sha256" || return 1
    verify_sidecar "$manifest" "$manifest_sha" || return 1
    python3 - "$image" "$manifest" <<'PY'
import hashlib
import json
import os
import sys

image, manifest_path = sys.argv[1:]
try:
    with open(manifest_path, encoding="utf-8") as stream:
        manifest = json.load(stream)
except (OSError, json.JSONDecodeError):
    raise SystemExit(1)

version = manifest.get("version")
artifact = manifest.get("artifact")
models = manifest.get("models")
if not isinstance(version, str) or not version or len(version) > 127:
    raise SystemExit(1)
if not isinstance(artifact, dict) or not isinstance(models, dict) or not models:
    raise SystemExit(1)
if artifact.get("filename") != os.path.basename(image):
    raise SystemExit(1)
if artifact.get("bytes") != os.path.getsize(image):
    raise SystemExit(1)
h = hashlib.sha256()
with open(image, "rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        h.update(block)
digest = h.hexdigest()
if artifact.get("sha256", "").lower() != digest:
    raise SystemExit(1)
allowed = {"validated", "untested", "known-incompatible", "confirmed"}
if any(not isinstance(key, str) or value not in allowed for key, value in models.items()):
    raise SystemExit(1)
PY
}

expected_rootfs_hash() {
    if [[ $2 == full ]]; then
        dd if="$1" bs=1048576 skip=3 count=8 status=none | sha256sum | awk '{print $1}'
    else
        python3 - "$1" <<'PY'
import hashlib
import os
import sys
path = sys.argv[1]
limit = 8 * 1024 * 1024
size = os.path.getsize(path)
if not 0 < size <= limit:
    raise SystemExit("invalid SquashFS size")
h = hashlib.sha256()
with open(path, "rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        h.update(block)
h.update(b"\xff" * (limit - size))
print(h.hexdigest())
PY
    fi
}

select_firmware() {
    local files=() types=() sizes=() file type size status i choice
    [[ -d $ARTIFACTS_DIR ]] || die "artifact directory not found: $ARTIFACTS_DIR"

    if [[ -n $SELECTED_FIRMWARE ]]; then
        [[ -f $SELECTED_FIRMWARE ]] || die "firmware not found: $SELECTED_FIRMWARE"
        SELECTED_FIRMWARE=$(readlink -f -- "$SELECTED_FIRMWARE")
        SELECTED_TYPE=$(classify_firmware "$SELECTED_FIRMWARE") || die 'selected file is not a valid full image or SquashFS'
        case $MODE in
            modern)
                validate_manifest_bundle "$SELECTED_FIRMWARE" ||
                    die 'modern mode requires image, image checksum, manifest, and manifest checksum'
                ;;
            checksum)
                validate_checksum_bundle "$SELECTED_FIRMWARE" ||
                    die 'checksum-only mode requires a valid image SHA-256 sidecar'
                ;;
            legacy)
                [[ $SELECTED_TYPE != raw-full ]] ||
                    die 'legacy updater mode cannot install an unknown full-flash layout'
                ;;
        esac
        return
    fi

    while IFS= read -r -d '' file; do
        type=$(classify_firmware "$file") || continue
        case $MODE in
            modern) validate_manifest_bundle "$file" || continue ;;
            checksum) validate_checksum_bundle "$file" || continue ;;
            legacy) [[ $type != raw-full ]] || continue ;;
        esac
        files+=("$file")
        types+=("$type")
        sizes+=("$(stat -c %s -- "$file")")
    done < <(find "$ARTIFACTS_DIR" -maxdepth 1 -type f -name '*.bin' -print0 | sort -z)

    if ((${#files[@]} == 0)); then
        case $MODE in
            modern) die "no complete modern artifact sets were found in $ARTIFACTS_DIR" ;;
            checksum) die "no valid image + .sha256 artifact sets were found in $ARTIFACTS_DIR" ;;
            legacy) die "no valid legacy firmware images were found in $ARTIFACTS_DIR" ;;
        esac
    fi

    printf '\nAvailable %s firmware artifacts:\n' "$MODE"
    for i in "${!files[@]}"; do
        case $MODE in modern) status=manifest ;; checksum) status=checksum ;; legacy) status=legacy ;; esac
        printf '  %d) %-9s %-10s %-8s %s\n' "$((i + 1))" "${types[$i]}" \
            "$(human_size "${sizes[$i]}")" "$status" "$(basename -- "${files[$i]}")"
    done
    while :; do
        read -r -p 'Select firmware number: ' choice
        [[ $choice =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#files[@]} )) || {
            warn 'invalid selection'
            continue
        }
        SELECTED_FIRMWARE=${files[$((choice - 1))]}
        SELECTED_TYPE=${types[$((choice - 1))]}
        return
    done
}

select_flash_scope() {
    local choice
    if [[ $MODE == legacy ]]; then
        [[ $FLASH_SCOPE == system ]] || die 'full-flash mode is unavailable with the legacy updater'
        return
    fi
    if [[ $SELECTED_TYPE == raw-full ]]; then
        [[ $FLASH_SCOPE != system || $FLASH_SCOPE_REQUESTED -eq 0 ]] ||
            die 'a raw full-flash image cannot be installed with --system-flash'
        FLASH_SCOPE=full
        log 'raw 16 MiB image detected; forcing full-flash scope'
        return
    fi
    if [[ $SELECTED_TYPE == squashfs ]]; then
        [[ $FLASH_SCOPE != full ]] || die 'full-flash mode requires an exact 16 MiB image'
        FLASH_SCOPE=system
        return
    fi
    if (( FLASH_SCOPE_REQUESTED )); then
        return 0
    fi
    printf '
Flash scope:
'
    printf '  1) system - update SquashFS and the selected JFFS2 policy only
'
    printf '  2) full   - overwrite loader, kernel, SquashFS, and JFFS2
'
    while :; do
        read -r -p 'Select flash scope [1]: ' choice
        case ${choice:-1} in
            1) FLASH_SCOPE=system; return ;;
            2) FLASH_SCOPE=full; return ;;
            *) warn 'invalid selection' ;;
        esac
    done
}

prepare_version_hint() {
    local detected
    if [[ $MODE != checksum ]]; then
        return 0
    fi
    if [[ -n $FIRMWARE_VERSION ]]; then
        return 0
    fi
    detected=$(detect_embedded_version "$SELECTED_FIRMWARE" "$SELECTED_TYPE" 2>/dev/null || true)
    if [[ -n $detected ]]; then
        FIRMWARE_VERSION=$detected
        log "using embedded firmware version: $FIRMWARE_VERSION"
        return
    fi
    read -r -p 'Firmware version hint (blank lets the target infer it): ' FIRMWARE_VERSION
    [[ ${#FIRMWARE_VERSION} -le 127 ]] || die 'version hint is too long'
}

confirm_full_flash() {
    local answer
    if [[ $FLASH_SCOPE != full || $OPERATION != flash ]]; then
        return 0
    fi
    printf '
WARNING: full-flash mode overwrites the bootloader and kernel in addition to rootfs/config.
'
    printf 'A power loss or incompatible bootloader can make the switch unbootable without an external programmer.
'
    read -r -p 'Type FLASH-ALL to authorize this host-side operation: ' answer
    [[ $answer == FLASH-ALL ]] || die 'full-flash authorization was not provided'
}

activate_bootloader_recovery() {
    [[ $MODE == modern ]] || die 'bootloader UART recovery requires the modern manifest-aware artifact contract'
    if [[ $OPERATION != preflight ]]; then
        [[ $SELECTED_TYPE == full ]] || \
            die 'bootloader UART recovery requires a supported 16 MiB postmerkOS full image'
    fi
    BOOTLOADER_RECOVERY=1
    CONTROL_PATH=bootloader
    if [[ $FLASH_SCOPE != full ]]; then
        log 'bootloader UART recovery always writes the complete 16 MiB SPI image; forcing full-flash scope'
    fi
    FLASH_SCOPE=full
    FLASH_SCOPE_REQUESTED=1
    OVERLAY_POLICY=image
}

select_bootloader_recovery_path() {
    local choice
    printf '
Bootloader recovery entry path:
'
    printf '  1) Upload corrected recovery utility through RAM loader (menu option 1; recommended for v0.7.0)
'
    printf '  2) Embedded recovery utility (menu option 2; requires fixed-entry loader)
'
    printf '  3) Try embedded recovery, then wait for reset and fall back to RAM upload
'
    while :; do
        read -r -p 'Select recovery path [1]: ' choice
        case ${choice:-1} in
            1) BOOTLOADER_RECOVERY_PATH=ram-upload; return ;;
            2) BOOTLOADER_RECOVERY_PATH=embedded; return ;;
            3) BOOTLOADER_RECOVERY_PATH=auto; return ;;
            *) warn 'invalid selection' ;;
        esac
    done
}

select_control_path() {
    local choice
    [[ -z $CONTROL_PATH ]] || return 0
    printf '\nFirmware upload/control path:\n'
    printf '  1) SSH to running postmerkOS (TFTP firmware source)\n'
    printf '  2) Hardware serial to running postmerkOS (TFTP or PMOSUART/1)\n'
    if [[ $MODE == modern && $SELECTED_TYPE == full ]]; then
        printf '  3) meraki-redboot UART recovery (pre-kernel full-image upload)\n'
    fi
    while :; do
        read -r -p 'Select upload/control path [1]: ' choice
        case ${choice:-1} in
            1) CONTROL_PATH=ssh; return ;;
            2) CONTROL_PATH=serial; return ;;
            3)
                if [[ $MODE != modern || $SELECTED_TYPE != full ]]; then
                    warn 'bootloader UART recovery requires a modern 16 MiB full image'
                    continue
                fi
                activate_bootloader_recovery
                select_bootloader_recovery_path
                return
                ;;
            *) warn 'invalid selection' ;;
        esac
    done
}

select_operation() {
    local choice
    (( OPERATION_PRESELECTED == 0 )) || return 0
    printf '\nOperation:\n'
    printf '  1) Verify download, checksum, manifest/metadata, board, and layout\n'
    printf '  2) Dry run; stage and prepare without stopping services or writing flash\n'
    printf '  3) Flash firmware and reboot\n'
    printf '  4) Force flash/reinstall/downgrade and reboot\n'
    printf '  5) Preflight pre-boot UART + SPI NOR read/erase/program/readback/restore\n'
    while :; do
        read -r -p 'Select operation [1]: ' choice
        case ${choice:-1} in
            1) OPERATION=verify; OPERATION_ARGS=(--verify-only); return ;;
            2) OPERATION=dry-run; OPERATION_ARGS=(--dry-run); return ;;
            3) OPERATION=flash; OPERATION_ARGS=(); return ;;
            4) OPERATION=flash; OPERATION_ARGS=(--force); FORCE_FLASH=1; return ;;
            5) OPERATION=preflight; OPERATION_ARGS=(); BOOTLOADER_RECOVERY=1; CONTROL_PATH=bootloader; return ;;
            *) warn 'invalid selection' ;;
        esac
    done
}

select_overlay_policy() {
    local choice
    if [[ $FLASH_SCOPE == full ]]; then
        OVERLAY_POLICY=image
        return
    fi
    printf '
Writable-overlay policy:
'
    printf '  1) preserve - leave JFFS2 byte-for-byte unchanged
'
    printf '  2) migrate  - copy preserve.list paths into a clean JFFS2 image
'
    printf '  3) reset    - flash a clean JFFS2 overlay
'
    [[ $SELECTED_TYPE == full ]] && printf '  4) image    - use the JFFS2 region embedded in the full image
'
    while :; do
        read -r -p 'Select overlay policy [1]: ' choice
        case ${choice:-1} in
            1) OVERLAY_POLICY=preserve; return ;;
            2) OVERLAY_POLICY=migrate; return ;;
            3) OVERLAY_POLICY=reset; return ;;
            4) [[ $SELECTED_TYPE == full ]] && { OVERLAY_POLICY=image; return; }; warn 'image policy requires a full 16 MiB image' ;;
            *) warn 'invalid selection' ;;
        esac
    done
}

stage_firmware() {
    local name hash
    TFTP_ROOT="$TMP/tftp-root"
    mkdir -p "$TFTP_ROOT"
    name=$(basename -- "$SELECTED_FIRMWARE")
    [[ $name =~ ^[A-Za-z0-9._-]+$ ]] || die "unsafe artifact filename: $name"
    STAGED_NAME=$name
    cp -- "$SELECTED_FIRMWARE" "$TFTP_ROOT/$STAGED_NAME"

    case $MODE in
        modern)
            validate_manifest_bundle "$SELECTED_FIRMWARE" || die 'modern artifact bundle failed validation during staging'
            cp -- "$SELECTED_FIRMWARE.sha256" "$TFTP_ROOT/$STAGED_NAME.sha256"
            cp -- "$SELECTED_FIRMWARE.manifest.json" "$TFTP_ROOT/$STAGED_NAME.manifest.json"
            cp -- "$SELECTED_FIRMWARE.manifest.json.sha256" "$TFTP_ROOT/$STAGED_NAME.manifest.json.sha256"
            ;;
        checksum)
            validate_checksum_bundle "$SELECTED_FIRMWARE" || die 'checksum-only artifact set failed validation during staging'
            cp -- "$SELECTED_FIRMWARE.sha256" "$TFTP_ROOT/$STAGED_NAME.sha256"
            ;;
        legacy)
            (cd "$TFTP_ROOT" && sha256sum "$STAGED_NAME" > "$STAGED_NAME.sha256")
            ;;
    esac

    hash=$(sha256sum "$TFTP_ROOT/$STAGED_NAME" | awk '{print $1}')
    EXPECTED_IMMUTABLE_HASH=""
    EXPECTED_ROOTFS_HASH=""
    if [[ $FLASH_SCOPE == full ]]; then
        EXPECTED_IMMUTABLE_HASH=$(head -c $((0xb00000)) "$SELECTED_FIRMWARE" | sha256sum | awk '{print $1}')
        log "expected loader+kernel+SquashFS SHA-256: $EXPECTED_IMMUTABLE_HASH"
    else
        EXPECTED_ROOTFS_HASH=$(expected_rootfs_hash "$SELECTED_FIRMWARE" "$SELECTED_TYPE")
        log "expected installed SquashFS-region SHA-256: $EXPECTED_ROOTFS_HASH"
    fi
    log "staged $STAGED_NAME using the $MODE contract"
    log "firmware SHA-256: $hash"
}

start_tftp_server() {
    local bind_ip=$1
    python3 "$SCRIPT_DIR/tftp-server.py" --bind "$bind_ip" --port "$TFTP_PORT" --root "$TFTP_ROOT" &
    TFTP_PID=$!
    sleep 0.4
    kill -0 "$TFTP_PID" 2>/dev/null || die "TFTP server failed to start on $bind_ip:$TFTP_PORT"
}

select_host_ip() {
    local preferred=${1:-} entries=() iface addr choice
    while read -r iface addr; do
        [[ -n $addr && $addr != 127.* ]] || continue
        entries+=("$iface|$addr")
    done < <(ip -o -4 addr show | awk '$2 != "lo" {split($4,a,"/"); print $2, a[1]}')
    if [[ -n $preferred ]]; then
        for choice in "${entries[@]}"; do
            [[ ${choice#*|} == "$preferred" ]] && { printf '%s' "$preferred"; return; }
        done
    fi
    ((${#entries[@]})) || die 'no non-loopback IPv4 address is available for TFTP'
    printf '\nHost addresses:\n' >&2
    for choice in "${!entries[@]}"; do
        IFS='|' read -r iface addr <<< "${entries[$choice]}"
        printf '  %d) %-12s %s\n' "$((choice + 1))" "$iface" "$addr" >&2
    done
    printf '  m) Enter an address manually\n' >&2
    while :; do
        read -r -p 'Select the address reachable by the switch: ' choice
        if [[ $choice == m || $choice == M ]]; then
            read -r -p 'Host IPv4 address: ' addr
            [[ $addr =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || { warn 'invalid IPv4 syntax'; continue; }
            printf '%s' "$addr"
            return
        fi
        [[ $choice =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#entries[@]} )) || { warn 'invalid selection'; continue; }
        printf '%s' "${entries[$((choice - 1))]#*|}"
        return
    done
}

remote_command() {
    local args
    case $MODE in
        modern)
            args=(postmerkos-console firmware tftp --server "$HOST_TFTP_IP" --port "$TFTP_PORT" \
                --file "$STAGED_NAME" --checksum-file "$STAGED_NAME.sha256" \
                --manifest-file "$STAGED_NAME.manifest.json" --overlay "$OVERLAY_POLICY")
            ;;
        checksum)
            args=(postmerkos-console firmware tftp --server "$HOST_TFTP_IP" --port "$TFTP_PORT" \
                --file "$STAGED_NAME" --checksum-file "$STAGED_NAME.sha256" \
                --no-manifest --overlay "$OVERLAY_POLICY")
            [[ -n $FIRMWARE_VERSION ]] && args+=(--version "$FIRMWARE_VERSION")
            ;;
        legacy)
            args=(fw_update_tftp --server "$HOST_TFTP_IP" --port "$TFTP_PORT" \
                --file "$STAGED_NAME" --checksum-file "$STAGED_NAME.sha256" --overlay "$OVERLAY_POLICY")
            ;;
    esac
    [[ $FLASH_SCOPE == full ]] && args+=(--full-flash --accept-full-flash)
    args+=("${OPERATION_ARGS[@]}")
    [[ $MODE == legacy ]] && args+=(--yes)
    printf '%q ' "${args[@]}"
}

remote_uart_command() {
    local args=(postmerkos-console firmware uart --overlay "$OVERLAY_POLICY")
    [[ $FLASH_SCOPE == full ]] && args+=(--full-flash --accept-full-flash)
    args+=("${OPERATION_ARGS[@]}")
    printf '%q ' "${args[@]}"
}

secret_lookup() {
    command -v secret-tool >/dev/null 2>&1 || return 1
    secret-tool lookup service meraki-fw-flasher host "$SSH_TARGET" user "$SSH_USER" port "$SSH_PORT" 2>/dev/null
}

secret_store() {
    command -v secret-tool >/dev/null 2>&1 || return 1
    printf '%s' "$SSH_PASSWORD" | secret-tool store \
        --label="Meraki firmware flasher: $SSH_USER@$SSH_TARGET:$SSH_PORT" \
        service meraki-fw-flasher host "$SSH_TARGET" user "$SSH_USER" port "$SSH_PORT"
}

prepare_askpass() {
    printf '%s' "$SSH_PASSWORD" > "$TMP/ssh-password"
    chmod 600 "$TMP/ssh-password"
    cat > "$TMP/ssh-askpass" <<EOF
#!/bin/sh
cat '$TMP/ssh-password'
EOF
    chmod 700 "$TMP/ssh-askpass"
}

ssh_options() {
    SSH_OPTS=(-p "$SSH_PORT" -o ConnectTimeout=10 -o ServerAliveInterval=5 \
        -o ServerAliveCountMax=3 -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile="$KNOWN_HOSTS" -o GlobalKnownHostsFile=/dev/null -o LogLevel=ERROR)
}

ssh_exec() {
    local quiet=0
    [[ ${1:-} == --quiet ]] && { quiet=1; shift; }
    if [[ $SSH_AUTH_MODE == key ]]; then
        if (( quiet )); then
            ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$SSH_USER@$SSH_TARGET" "$@" >/dev/null 2>&1
        else
            ssh "${SSH_OPTS[@]}" "$SSH_USER@$SSH_TARGET" "$@"
        fi
    else
        prepare_askpass
        if (( quiet )); then
            DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$TMP/ssh-askpass" \
                setsid -w ssh "${SSH_OPTS[@]}" "$SSH_USER@$SSH_TARGET" "$@" >/dev/null 2>&1
        else
            DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$TMP/ssh-askpass" \
                setsid -w ssh "${SSH_OPTS[@]}" "$SSH_USER@$SSH_TARGET" "$@"
        fi
    fi
}

ssh_exec_interactive() {
    if [[ $SSH_AUTH_MODE == key ]]; then
        ssh -tt "${SSH_OPTS[@]}" "$SSH_USER@$SSH_TARGET" "$@"
    else
        prepare_askpass
        DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS="$TMP/ssh-askpass" \
            setsid -w ssh -tt "${SSH_OPTS[@]}" "$SSH_USER@$SSH_TARGET" "$@"
    fi
}

prepare_ssh_auth() {
    local saved=""
    mkdir -p "$CONFIG_DIR" "$LOG_DIR"
    chmod 700 "$CONFIG_DIR"
    touch "$KNOWN_HOSTS"
    chmod 600 "$KNOWN_HOSTS"
    ssh_options
    if ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$SSH_USER@$SSH_TARGET" true >/dev/null 2>&1; then
        SSH_AUTH_MODE=key
        log 'SSH key authentication succeeded'
        return
    fi
    SSH_AUTH_MODE=password
    if saved=$(secret_lookup) && [[ -n $saved ]] && prompt_yes_no "Use saved keyring password for $SSH_USER@$SSH_TARGET?" y; then
        SSH_PASSWORD=$saved
    fi
    if [[ -z $SSH_PASSWORD ]]; then
        read -r -s -p "SSH password for $SSH_USER@$SSH_TARGET: " SSH_PASSWORD
        printf '\n'
    fi
    [[ -n $SSH_PASSWORD ]] || die 'SSH password may not be empty'
    ssh_exec --quiet true || die 'SSH authentication failed'
    if command -v secret-tool >/dev/null 2>&1 && prompt_yes_no 'Save this password in the desktop keyring?' y; then
        secret_store && log 'password saved in the desktop keyring' || warn 'desktop keyring rejected the password'
    fi
}

clear_expected_changed_host_key() {
    if [[ $FLASH_SCOPE != full && $OVERLAY_POLICY != reset && $OVERLAY_POLICY != image ]]; then
        return 0
    fi
    if ! command -v ssh-keygen >/dev/null 2>&1; then
        return 0
    fi
    if [[ $SSH_PORT == 22 ]]; then
        ssh-keygen -q -R "$SSH_TARGET" -f "$KNOWN_HOSTS" >/dev/null 2>&1 || true
    else
        ssh-keygen -q -R "[$SSH_TARGET]:$SSH_PORT" -f "$KNOWN_HOSTS" >/dev/null 2>&1 || true
    fi
}

wait_for_ssh_return() {
    local deadline=$((SECONDS + 900)) announced=0
    clear_expected_changed_host_key
    while (( SECONDS < deadline )); do
        if ssh_exec --quiet true; then
            log 'switch is reachable by SSH again'
            return
        fi
        (( announced )) || { log 'switch is rebooting or SSH is temporarily unavailable'; announced=1; }
        sleep 5
    done
    return 1
}

remote_rootfs_hash() {
    ssh_exec 'mtd=$(awk '\''$4=="\\"squashfs\\"" || $4=="\\"rootfs\\"" {sub(/:$/,"",$1); print $1; exit}'\'' /proc/mtd); [ -n "$mtd" ] && dd if=/dev/$mtd bs=1048576 count=8 2>/dev/null | sha256sum | awk '\''{print $1}'\'''
}

remote_immutable_hash() {
    local script
    read -r -d '' script <<'REMOTE_FULL_HASH' || true
set -e
find_mtd() {
    size=$1
    shift
    for name in "$@"; do
        dev=$(awk -v n="\"$name\"" -v s="$size" '$2 == s && $4 == n { sub(/:$/, "", $1); print $1; exit }' /proc/mtd)
        [ -n "$dev" ] && { echo "$dev"; return; }
    done
    awk -v s="$size" '$2 == s { sub(/:$/, "", $1); found=$1; count++ } END { if (count == 1) print found }' /proc/mtd
}
loader=$(find_mtd 00040000 RedBoot redboot bootloader loader u-boot uboot)
kernel=$(find_mtd 002c0000 kernel linux vmlinux boot1)
root=$(find_mtd 00800000 squashfs rootfs)
[ -n "$loader" ] && [ -n "$kernel" ] && [ -n "$root" ]
{
    dd if=/dev/$loader bs=65536 count=4 2>/dev/null
    dd if=/dev/$kernel bs=65536 count=44 2>/dev/null
    dd if=/dev/$root bs=1048576 count=8 2>/dev/null
} | sha256sum | awk '{print $1}'
REMOTE_FULL_HASH
    ssh_exec "$script"
}

find_serial_console_tool() {
    local candidate="$REPO_ROOT/tools/serial-console/serial-console.sh"
    [[ -x $candidate ]] && printf '%s' "$candidate"
}

offer_serial_console() {
    local tool
    tool=$(find_serial_console_tool) || return 0
    prompt_yes_no 'Open the separate serial-console tool for diagnosis?' y && "$tool" || true
}

run_ssh_mode() {
    local route_ip command rc=0 actual_hash=""
    SSH_TARGET=$(prompt_default 'Switch IP address or hostname' '169.254.0.10')
    SSH_USER=$(prompt_default 'SSH username' 'root')
    SSH_PORT=$(prompt_default 'SSH port' '22')
    [[ $SSH_PORT =~ ^[0-9]+$ ]] && (( SSH_PORT >= 1 && SSH_PORT <= 65535 )) || die 'invalid SSH port'
    route_ip=$(ip route get "$SSH_TARGET" 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}' || true)
    HOST_TFTP_IP=$(select_host_ip "$route_ip")

    prepare_ssh_auth
    stage_firmware
    start_tftp_server "$HOST_TFTP_IP"
    command=$(remote_command)

    printf '\nMode:              %s\n' "$MODE"
    if [[ $OPERATION == preflight ]]; then
        printf 'Scratch sector:    %s (64 KiB; bootloader protected; restored after test)\n' "$PREFLIGHT_SCRATCH"
    else
        printf 'Selected firmware: %s\n' "$SELECTED_FIRMWARE"
    fi
    printf 'Control path:      SSH to %s@%s:%s\n' "$SSH_USER" "$SSH_TARGET" "$SSH_PORT"
    if [[ $TRANSPORT == uart ]]; then printf 'Firmware source:   UART PMOSUART/1 (%s)\n' "$STAGED_NAME"; else printf 'TFTP source:       tftp://%s:%s/%s\n' "$HOST_TFTP_IP" "$TFTP_PORT" "$STAGED_NAME"; fi
    printf 'Operation:         %s\n' "$OPERATION"
    printf 'Flash scope:       %s\n' "$FLASH_SCOPE"
    printf 'Overlay policy:    %s\n\n' "$OVERLAY_POLICY"

    if [[ $MODE != legacy ]]; then
        printf '%s\n' 'The current updater may request ACCEPT-UNTESTED and, for flashing, UPGRADE.' \
            'Those prompts are deliberate and will appear in the SSH session below.'
    fi
    if [[ $OPERATION == flash ]] && ! prompt_yes_no 'Start the firmware flash now?' n; then
        die 'cancelled'
    fi

    set +e
    if [[ $MODE != legacy ]]; then
        ssh_exec_interactive "$command"
    else
        ssh_exec "$command"
    fi
    rc=$?
    set -e

    if [[ $OPERATION != flash ]]; then
        (( rc == 0 )) || { offer_serial_console; die "remote updater exited with status $rc"; }
        log "$OPERATION completed successfully; flash was not modified"
        return
    fi

    if (( rc != 0 )); then
        if ssh_exec --quiet true; then
            offer_serial_console
            die "remote updater exited with status $rc before the switch rebooted"
        fi
        warn "SSH ended with status $rc after management services stopped; waiting for reboot"
    fi
    if ! wait_for_ssh_return; then
        offer_serial_console
        die 'switch did not return to SSH after the update'
    fi

    set +e
    if [[ $FLASH_SCOPE == full ]]; then
        actual_hash=$(remote_immutable_hash 2>/dev/null | tr -d '\r\n')
    else
        actual_hash=$(remote_rootfs_hash 2>/dev/null | tr -d '\r\n')
    fi
    set -e
    if [[ $FLASH_SCOPE == full && $actual_hash == "$EXPECTED_IMMUTABLE_HASH" ]]; then
        log 'post-reboot loader, kernel, and SquashFS hash matches the selected firmware'
    elif [[ $FLASH_SCOPE == system && $actual_hash == "$EXPECTED_ROOTFS_HASH" ]]; then
        log 'post-reboot SquashFS readback hash matches the selected firmware'
    elif [[ -n $actual_hash ]]; then
        if [[ $FLASH_SCOPE == full ]]; then
            warn "post-reboot immutable-region hash mismatch: expected $EXPECTED_IMMUTABLE_HASH, got $actual_hash"
        else
            warn "post-reboot SquashFS hash mismatch: expected $EXPECTED_ROOTFS_HASH, got $actual_hash"
        fi
        return 1
    else
        warn "switch returned, but the installed $FLASH_SCOPE flash scope could not be hashed"
    fi
    ssh_exec 'cat /etc/lsb-release 2>/dev/null || true; uname -a'
}

select_serial_device() {
    local devices=(/dev/serial/by-id/* /dev/ttyUSB* /dev/ttyACM*) unique=() dev existing choice i
    if [[ -n $SERIAL_DEVICE ]]; then
        [[ -c $SERIAL_DEVICE ]] || die "serial device not found: $SERIAL_DEVICE"
        return
    fi
    for dev in "${devices[@]}"; do
        [[ -c $dev ]] || continue
        for existing in "${unique[@]}"; do
            [[ $(readlink -f -- "$existing") == "$(readlink -f -- "$dev")" ]] && continue 2
        done
        unique+=("$dev")
    done
    ((${#unique[@]})) || die 'no USB serial device was found'
    printf '\nSerial devices:\n'
    for i in "${!unique[@]}"; do printf '  %d) %s\n' "$((i + 1))" "${unique[$i]}"; done
    printf '  m) Enter a device manually\n'
    while :; do
        read -r -p 'Select serial device: ' choice
        if [[ $choice == m || $choice == M ]]; then
            read -r -p 'Serial device path: ' SERIAL_DEVICE
            [[ -c $SERIAL_DEVICE ]] || { warn 'not a character device'; continue; }
            return
        fi
        [[ $choice =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#unique[@]} )) || { warn 'invalid selection'; continue; }
        SERIAL_DEVICE=${unique[$((choice - 1))]}
        return
    done
}

ensure_serial_access() {
    if [[ -r $SERIAL_DEVICE && -w $SERIAL_DEVICE ]]; then
        return 0
    fi
    warn "current user cannot read and write $SERIAL_DEVICE"
    if command -v setfacl >/dev/null 2>&1 && prompt_yes_no 'Grant temporary access with sudo setfacl?' y; then
        sudo setfacl -m "u:$USER:rw" "$SERIAL_DEVICE"
    fi
    [[ -r $SERIAL_DEVICE && -w $SERIAL_DEVICE ]] || die 'serial access remains unavailable'
}

run_serial_mode() {
    local command log_file rc accept_args=() runner_args=()
    select_serial_device
    ensure_serial_access
    SERIAL_USER=$(prompt_default 'Serial-console username' 'root')
    read -r -s -p "Serial-console password for $SERIAL_USER (blank when serial authentication is disabled): " SERIAL_PASSWORD
    printf '\n'
    if [[ $TRANSPORT == tftp ]]; then HOST_TFTP_IP=$(select_host_ip); fi

    if [[ $MODE != legacy ]] && prompt_yes_no 'Permit the runner to send ACCEPT-UNTESTED if the firmware requests it?' n; then
        ALLOW_UNTESTED=1
        accept_args=(--accept-untested)
    fi

    stage_firmware
    if [[ $TRANSPORT == uart ]]; then
        [[ $MODE != legacy ]] || die 'UART transport is unavailable for legacy firmware'
        command=$(remote_uart_command)
    else
        start_tftp_server "$HOST_TFTP_IP"
        command=$(remote_command)
    fi
    printf '%s' "$SERIAL_PASSWORD" > "$TMP/serial-password"
    chmod 600 "$TMP/serial-password"
    mkdir -p "$LOG_DIR"
    log_file="$LOG_DIR/firmware-flash-serial-$(date +%Y%m%d-%H%M%S).log"

    printf '\nMode:              %s\n' "$MODE"
    if [[ $OPERATION == preflight ]]; then
        printf 'Scratch sector:    %s (64 KiB; bootloader protected; restored after test)\n' "$PREFLIGHT_SCRATCH"
    else
        printf 'Selected firmware: %s\n' "$SELECTED_FIRMWARE"
    fi
    printf 'Control path:      serial %s, 115200 8N1 XON/XOFF\n' "$SERIAL_DEVICE"
    if [[ $TRANSPORT == uart ]]; then printf 'Firmware source:   UART PMOSUART/1 (%s)\n' "$STAGED_NAME"; else printf 'TFTP source:       tftp://%s:%s/%s\n' "$HOST_TFTP_IP" "$TFTP_PORT" "$STAGED_NAME"; fi
    printf 'Operation:         %s\n' "$OPERATION"
    printf 'Flash scope:       %s\n' "$FLASH_SCOPE"
    printf 'Overlay policy:    %s\n' "$OVERLAY_POLICY"
    printf 'Serial log:        %s\n\n' "$log_file"

    if [[ $OPERATION == flash ]] && ! prompt_yes_no 'Start the firmware flash now?' n; then
        die 'cancelled'
    fi

    [[ $FLASH_SCOPE == full ]] && accept_args+=(--accept-full-flash)
    set +e
    runner_args=()
    if [[ $TRANSPORT == uart ]]; then
        runner_args+=(--uart-firmware "$SELECTED_FIRMWARE")
        [[ $MODE == modern ]] && runner_args+=(--uart-manifest "$SELECTED_FIRMWARE.manifest.json")
    fi
    python3 "$SCRIPT_DIR/serial-runner.py" --device "$SERIAL_DEVICE" --username "$SERIAL_USER" \
        --password-file "$TMP/serial-password" --command "$command" --operation "$OPERATION" \
        --mode "$MODE" "${accept_args[@]}" "${runner_args[@]}" 2>&1 | tee "$log_file"
    rc=${PIPESTATUS[0]}
    set -e
    (( rc == 0 )) || die "serial-controlled update ended with status $rc; inspect $log_file"
    log "serial-controlled $OPERATION completed"
}

run_bootloader_recovery_mode() {
    local family default_payload preflight_receipt args=()
    need python3
    [[ -x $SCRIPT_DIR/bootloader-ramload.py ]] || die 'bootloader-ramload.py helper is missing or not executable'
    [[ -f $SCRIPT_DIR/bootloader_protocol.py ]] || die 'bootloader_protocol.py helper is missing'
    [[ -f $SCRIPT_DIR/pmosrec_v3.py ]] || die 'pmosrec_v3.py helper is missing'
    [[ $MODE == modern ]] || die 'bootloader recovery requires the modern manifest-aware artifact contract'
    if [[ $OPERATION != preflight ]]; then
        [[ $FLASH_SCOPE == full && $SELECTED_TYPE == full ]] || \
            die 'bootloader recovery requires a supported SPIM/SquashFS 16 MiB full image'
    fi
    [[ -n $TARGET_MODEL ]] || TARGET_MODEL=$(prompt_default 'Exact target model (for example MS42P or MS220-8P)' 'MS42P')
    case $TARGET_MODEL in
        MS22|MS22P|MS220-8|MS220-8P|MS220-24|MS220-24P) family=luton26 ;;
        MS320-24|MS320-24P|MS220-48|MS220-48P|MS220-48LP|MS220-48FP|MS320-48|MS320-48P|MS320-48LP|MS320-48FP|MS42|MS42P) family=jaguar1 ;;
        *) die "unsupported exact target model for pre-kernel recovery: $TARGET_MODEL" ;;
    esac
    default_payload="$ARTIFACTS_DIR/recovery/recovery-$family.bin"
    if [[ $BOOTLOADER_RECOVERY_PATH == ram-upload || $BOOTLOADER_RECOVERY_PATH == auto || -n $BOOTLOADER_PAYLOAD ]]; then
        [[ -n $BOOTLOADER_PAYLOAD ]] || BOOTLOADER_PAYLOAD=$default_payload
        [[ -f $BOOTLOADER_PAYLOAD ]] || die "target-specific external recovery payload not found: $BOOTLOADER_PAYLOAD"
    fi

    args=(
      --operation "$OPERATION"
      --recovery-path "$BOOTLOADER_RECOVERY_PATH"
      --target-model "$TARGET_MODEL"
    )
    if [[ $OPERATION == preflight ]]; then
        mkdir -p "$LOG_DIR"
        preflight_receipt="$LOG_DIR/bootloader-preflight-${TARGET_MODEL}-${family}.json"
        args+=(--preflight-scratch "$PREFLIGHT_SCRATCH" --preflight-receipt "$preflight_receipt")
    else
        args+=(--firmware "$SELECTED_FIRMWARE" --manifest "$SELECTED_FIRMWARE.manifest.json")
    fi
    if [[ -n $BOOTLOADER_PAYLOAD ]]; then
        args+=(--payload "$BOOTLOADER_PAYLOAD")
        local payload_descriptor="${BOOTLOADER_PAYLOAD%.bin}.descriptor.json"
        [[ -f $payload_descriptor ]] || die "corrected recovery payload descriptor not found: $payload_descriptor"
        args+=(--payload-descriptor "$payload_descriptor")
    fi
    (( FORCE_FLASH )) && args+=(--force)
    [[ $OPERATION == flash ]] && args+=(--host-full-flash-authorized)
    (( MANUAL_TARGET_CONFIRMATION )) && args+=(--manual-target-confirmation)
    (( VERBOSE_ACKS )) && args+=(--verbose-acks)
    (( SKIP_BAUD_NEGOTIATION )) && args+=(--skip-baud-negotiation)
    (( DIAGNOSTIC_BAUD_SCAN )) && args+=(--diagnostic-baud-scan)
    (( DIAGNOSTIC_WINDOW_SCAN )) && args+=(--diagnostic-window-scan)

    printf '\nPre-kernel UART recovery\n'
    printf 'Operation:         %s\n' "$OPERATION"
    printf 'Target model:      %s (%s)\n' "$TARGET_MODEL" "$family"
    if [[ $OPERATION == preflight ]]; then
        printf 'Scratch sector:    %s (64 KiB; bootloader protected; restored after test)\n' "$PREFLIGHT_SCRATCH"
    else
        printf 'Selected firmware: %s\n' "$SELECTED_FIRMWARE"
    fi
    printf 'Recovery path:     %s\n' "$BOOTLOADER_RECOVERY_PATH"
    if [[ -n $BOOTLOADER_PAYLOAD ]]; then printf 'External payload:  %s\n' "$BOOTLOADER_PAYLOAD"; else printf 'External payload:  not required (embedded in meraki-redboot)\n'; fi

    if [[ $OPERATION == verify ]]; then
        python3 "$SCRIPT_DIR/bootloader-ramload.py" "${args[@]}"
        return
    fi

    select_serial_device
    ensure_serial_access
    args+=(--port "$SERIAL_DEVICE")
    printf 'Serial device:     %s, 115200 8N1, binary transport\n\n' "$SERIAL_DEVICE"
    if [[ $OPERATION == flash ]]; then
        prompt_yes_no 'Begin recovery upload and target-side validation?' n || die 'cancelled'
    elif [[ $OPERATION == preflight ]]; then
        printf 'The selected scratch sector will be backed up, erased, fully programmed, verified, and restored.\n'
        prompt_yes_no 'Begin destructive-but-restored UART/NOR preflight?' n || die 'cancelled'
    else
        prompt_yes_no 'Begin non-destructive recovery dry-run?' y || die 'cancelled'
    fi
    mkdir -p "$LOG_DIR"
    local log_file rc
    log_file="$LOG_DIR/bootloader-uart-recovery-$(date +%Y%m%d-%H%M%S).log"
    printf 'Recovery log:      %s\n' "$log_file"
    set +e
    SUPPRESS_ERR_REPORT=1
    python3 "$SCRIPT_DIR/bootloader-ramload.py" "${args[@]}" 2>&1 | tee "$log_file"
    rc=${PIPESTATUS[0]}
    SUPPRESS_ERR_REPORT=0
    set -e
    (( rc == 0 )) || die "bootloader UART recovery ended with status $rc; inspect $log_file"
    log 'bootloader UART recovery completed'
}

self_test() {
    local test_image manifest
    TMP=$(mktemp -d)
    chmod 700 "$TMP"
    TFTP_ROOT="$TMP/tftp-root"
    mkdir -p "$TFTP_ROOT"
    printf 'postmerkOS TFTP self-test\n' > "$TFTP_ROOT/test.bin"
    TFTP_PORT=$((20000 + RANDOM % 20000))
    start_tftp_server 127.0.0.1
    python3 - "$TFTP_PORT" <<'PY'
import socket
import struct
import sys
port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(3)
sock.sendto(struct.pack('!H', 1) + b'test.bin\0octet\0', ('127.0.0.1', port))
data, peer = sock.recvfrom(65535)
opcode, block = struct.unpack('!HH', data[:4])
assert opcode == 3 and block == 1 and b'postmerkOS TFTP self-test' in data[4:]
sock.sendto(struct.pack('!HH', 4, block), peer)
PY
    stop_tftp_server

    test_image="$TMP/test.bin"
    truncate -s 16777216 "$test_image"
    printf SPIM | dd of="$test_image" bs=1 seek=$((0x40000)) conv=notrunc status=none
    printf hsqs | dd of="$test_image" bs=1 seek=$((0x300000)) conv=notrunc status=none
    (cd "$TMP" && sha256sum test.bin > test.bin.sha256)
    manifest="$test_image.manifest.json"
    python3 - "$test_image" "$manifest" <<'PY'
import hashlib, json, os, sys
image, output = sys.argv[1:]
h = hashlib.sha256()
with open(image, 'rb') as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        h.update(block)
digest = h.hexdigest()
with open(output, 'w', encoding='utf-8') as stream:
    json.dump({'version': 'self-test', 'artifact': {'filename': os.path.basename(image), 'bytes': os.path.getsize(image), 'sha256': digest}, 'models': {'MS42P': 'validated'}}, stream)
    stream.write('\n')
PY
    (cd "$TMP" && sha256sum test.bin.manifest.json > test.bin.manifest.json.sha256)
    validate_manifest_bundle "$test_image" || die 'modern bundle self-test failed'
    validate_checksum_bundle "$test_image" || die 'checksum-only bundle self-test failed'
    [[ $(classify_firmware "$test_image") == full ]] || die 'current full-image classification self-test failed'
    cp "$test_image" "$TMP/alternate-boot.bin"
    printf UBT0 | dd of="$TMP/alternate-boot.bin" bs=1 seek=$((0x40000)) conv=notrunc status=none
    [[ $(classify_firmware "$TMP/alternate-boot.bin") == raw-full ]] || die 'alternate boot-chain classification self-test failed'
    if [[ ${FIRMWARE_FLASHER_SELFTEST_QUICK:-0} != 1 ]]; then
        python3 - "$SCRIPT_DIR/tftp-server.py" "$SCRIPT_DIR/serial-runner.py" "$SCRIPT_DIR/bootloader-ramload.py" "$SCRIPT_DIR/bootloader_protocol.py" "$SCRIPT_DIR/pmosrec_v3.py" <<'PY_SYNTAX'
from pathlib import Path
import sys
for source in sys.argv[1:]:
    compile(Path(source).read_text(encoding='utf-8'), source, 'exec')
PY_SYNTAX
        python3 -m unittest discover -s "$SCRIPT_DIR/tests" -p 'test_*.py' -v
    fi
    log 'TFTP, artifact bundles, image classification, and UART recovery protocol self-tests passed'
}

main() {
    local self_test_requested=0
    while (($#)); do
        case $1 in
            --artifacts) (($# >= 2)) || die '--artifacts requires a directory'; ARTIFACTS_DIR=$2; shift 2 ;;
            --firmware) (($# >= 2)) || die '--firmware requires a file'; SELECTED_FIRMWARE=$2; shift 2 ;;
            --tftp-port) (($# >= 2)) || die '--tftp-port requires a port'; TFTP_PORT=$2; shift 2 ;;
            --control) (($# >= 2)) || die '--control requires ssh or serial'; CONTROL_PATH=$2; shift 2 ;;
            --transport) (($# >= 2)) || die '--transport requires tftp or uart'; TRANSPORT=$2; shift 2 ;;
            --bootloader-recovery) BOOTLOADER_RECOVERY=1; CONTROL_PATH=bootloader; FLASH_SCOPE=full; FLASH_SCOPE_REQUESTED=1; shift ;;
            --bootloader-preflight) BOOTLOADER_RECOVERY=1; CONTROL_PATH=bootloader; FLASH_SCOPE=full; FLASH_SCOPE_REQUESTED=1; OPERATION=preflight; OPERATION_PRESELECTED=1; shift ;;
            --recovery-path) (($# >= 2)) || die '--recovery-path requires embedded, auto, or ram-upload'; BOOTLOADER_RECOVERY_PATH=$2; shift 2 ;;
            --recovery-payload) (($# >= 2)) || die '--recovery-payload requires a file'; BOOTLOADER_PAYLOAD=$2; shift 2 ;;
            --target-model) (($# >= 2)) || die '--target-model requires a model'; TARGET_MODEL=$2; shift 2 ;;
            --preflight-scratch) (($# >= 2)) || die '--preflight-scratch requires an address'; PREFLIGHT_SCRATCH=$2; shift 2 ;;
            --manual-target-confirmation) MANUAL_TARGET_CONFIRMATION=1; shift ;;
            --verbose-acks) VERBOSE_ACKS=1; shift ;;
            --skip-baud-negotiation) SKIP_BAUD_NEGOTIATION=1; shift ;;
            --diagnostic-baud-scan) DIAGNOSTIC_BAUD_SCAN=1; shift ;;
            --diagnostic-window-scan) DIAGNOSTIC_WINDOW_SCAN=1; shift ;;
            --serial-device) (($# >= 2)) || die '--serial-device requires a device'; SERIAL_DEVICE=$2; shift 2 ;;
            --modern) MODE=modern; shift ;;
            --checksum-only|--original-artifacts) MODE=checksum; shift ;;
            --legacy) MODE=legacy; shift ;;
            --full-flash) FLASH_SCOPE=full; FLASH_SCOPE_REQUESTED=1; shift ;;
            --system-flash) FLASH_SCOPE=system; FLASH_SCOPE_REQUESTED=1; shift ;;
            --version) (($# >= 2)) || die '--version requires a value'; FIRMWARE_VERSION=$2; shift 2 ;;
            --self-test) self_test_requested=1; shift ;;
            --help|-h) usage; return ;;
            *) die "unknown option: $1" ;;
        esac
    done
    [[ $CONTROL_PATH == '' || $CONTROL_PATH == ssh || $CONTROL_PATH == serial || $CONTROL_PATH == bootloader ]] || die '--control must be ssh, serial, or bootloader'
    [[ $TRANSPORT == tftp || $TRANSPORT == uart ]] || die '--transport must be tftp or uart'
    [[ $MODE == modern || $MODE == checksum || $MODE == legacy ]] || die 'invalid firmware contract mode'
    [[ $TRANSPORT != uart || -z $CONTROL_PATH || $CONTROL_PATH == serial || $CONTROL_PATH == bootloader ]] || die '--transport uart requires --control serial or bootloader'
    [[ $TRANSPORT != uart || $MODE != legacy ]] || die '--transport uart is unavailable with --legacy'
    [[ $FLASH_SCOPE == system || $FLASH_SCOPE == full ]] || die 'invalid flash scope'
    [[ ${#FIRMWARE_VERSION} -le 127 && $FIRMWARE_VERSION != *$'\n'* && $FIRMWARE_VERSION != *$'\r'* ]] || die 'invalid version hint'
    [[ $MODE != legacy || $FLASH_SCOPE != full ]] || die '--full-flash is unavailable with --legacy'
    [[ $BOOTLOADER_RECOVERY -eq 0 || $MODE == modern ]] || die '--bootloader-recovery requires --modern'
    [[ $BOOTLOADER_RECOVERY_PATH == embedded || $BOOTLOADER_RECOVERY_PATH == auto || $BOOTLOADER_RECOVERY_PATH == ram-upload ]] || die '--recovery-path must be embedded, auto, or ram-upload'
    [[ $PREFLIGHT_SCRATCH =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]] || die '--preflight-scratch must be a numeric address'
    if [[ $CONTROL_PATH == bootloader ]]; then
        BOOTLOADER_RECOVERY=1
        FLASH_SCOPE=full
        FLASH_SCOPE_REQUESTED=1
    fi
    [[ $TFTP_PORT =~ ^[0-9]+$ ]] && (( TFTP_PORT >= 1024 && TFTP_PORT <= 65535 )) || die 'TFTP port must be from 1024 through 65535'

    for command in bash python3 sha256sum dd awk stat od tr readlink truncate head tee; do need "$command"; done
    [[ -x $SCRIPT_DIR/tftp-server.py && -x $SCRIPT_DIR/serial-runner.py ]] || die 'private flasher helpers are missing or not executable'
    (( self_test_requested )) && { self_test; return; }
    for command in find ip sort; do need "$command"; done

    TMP=$(mktemp -d)
    chmod 700 "$TMP"
    if [[ $OPERATION == preflight && $OPERATION_PRESELECTED -eq 1 ]]; then
        MODE=modern
        SELECTED_TYPE=preflight
        FLASH_SCOPE=full
        FLASH_SCOPE_REQUESTED=1
    else
        select_firmware
        select_flash_scope
        prepare_version_hint
        select_operation
    fi

    if (( BOOTLOADER_RECOVERY )); then
        activate_bootloader_recovery
    else
        select_control_path
    fi

    select_overlay_policy
    confirm_full_flash

    if (( BOOTLOADER_RECOVERY )); then
        run_bootloader_recovery_mode
        return
    fi

    [[ $TRANSPORT != uart || $CONTROL_PATH == serial ]] || die 'UART transport requires hardware serial control'
    case $CONTROL_PATH in
        ssh) need ssh; need setsid; run_ssh_mode ;;
        serial) run_serial_mode ;;
        *) die "unsupported control path: $CONTROL_PATH" ;;
    esac
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
