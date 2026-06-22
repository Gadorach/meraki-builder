#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
ARTIFACTS_DIR=${ARTIFACTS_DIR:-"$REPO_ROOT/artifacts"}
PROGRAMMER=${PROGRAMMER:-ch341a_spi}
CHIP=${CHIP:-MX25L12805D}
BACKUP_READS=${BACKUP_READS:-3}
EXPECTED_SIZE=$((16 * 1024 * 1024))
IMAGE=""
BACKUP_MODE=ask
OPEN_SERIAL=ask
VERIFY_FILE=""

usage() {
    cat <<'USAGE'
usage: backup-and-flash.sh [options] [FIRMWARE.bin]

Back up a 16 MiB MS42/MS220-family SPI NOR with flashrom, require matching
backup reads, flash a complete postmerkOS RedBoot/LinuxLoader image, and verify
it with a full readback comparison.

Options:
  --image FILE          complete 16 MiB firmware image
  --artifacts DIR       artifact directory (default: ../../artifacts)
  --programmer NAME     flashrom programmer (default: ch341a_spi)
  --chip NAME           flashrom chip name (default: MX25L12805D)
  --backup-reads N      matching backup reads required (default: 3, minimum: 2)
  --backup              require a verified backup without prompting
  --no-backup           explicitly request the confirmed no-backup path
  --serial              open serial-console.sh after a successful flash
  --no-serial           do not offer to open the serial console
  --help                show this help

The switch must remain unpowered while the SPI programmer is connected.
USAGE
}

say() { printf '\n%s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"; }

prompt_yes_no() {
    local prompt=$1 default=${2:-yes} answer suffix
    if [[ $default == yes ]]; then suffix='[Y/n]'; else suffix='[y/N]'; fi
    read -r -p "$prompt $suffix " answer || true
    answer=${answer:-$([[ $default == yes ]] && printf y || printf n)}
    [[ $answer =~ ^[Yy]([Ee][Ss])?$ ]]
}

pause_confirm() {
    printf '\n%s\n' "$1"
    read -r -p 'Press Enter when ready, or Ctrl+C to abort... ' _
}

read_magic_hex() {
    dd if="$1" bs=1 skip="$2" count=4 status=none 2>/dev/null |
        od -An -tx1 | tr -d ' \n'
}

resolve_image() {
    local latest candidate size kernel_magic rootfs_magic
    if [[ -z $IMAGE ]]; then
        latest="$ARTIFACTS_DIR/latest-image.txt"
        if [[ -f $latest ]]; then
            IFS= read -r candidate < "$latest" || true
            if [[ -n $candidate && $candidate != /* ]]; then
                candidate="$(cd -- "$(dirname -- "$latest")" && pwd)/$candidate"
            fi
            IMAGE=$candidate
        fi
    fi
    [[ -n $IMAGE && -f $IMAGE ]] || die "Pass a firmware image or provide $ARTIFACTS_DIR/latest-image.txt"
    IMAGE=$(readlink -f -- "$IMAGE")
    size=$(stat -c %s -- "$IMAGE")
    [[ $size -eq $EXPECTED_SIZE ]] || die "Firmware must be exactly $EXPECTED_SIZE bytes; got $size"

    kernel_magic=$(read_magic_hex "$IMAGE" $((0x40000)))
    rootfs_magic=$(read_magic_hex "$IMAGE" $((0x300000)))
    [[ $kernel_magic == 5350494d ]] || die "Missing SPIM kernel header at 0x40000"
    [[ $rootfs_magic == 68737173 ]] || die "Missing SquashFS magic at 0x300000"

    if [[ -f $IMAGE.sha256 ]]; then
        local listed expected actual
        expected=$(awk 'NF >= 2 && $1 !~ /^#/ {print tolower($1); exit}' "$IMAGE.sha256")
        listed=$(awk 'NF >= 2 && $1 !~ /^#/ {$1=""; sub(/^[ \t]+[*]?/, ""); sub(/\r$/, ""); print; exit}' "$IMAGE.sha256")
        [[ $expected =~ ^[0-9a-f]{64}$ ]] || die "Invalid SHA-256 sidecar: $IMAGE.sha256"
        [[ $listed == "$(basename -- "$IMAGE")" ]] || die "SHA-256 sidecar does not name $(basename -- "$IMAGE")"
        actual=$(sha256sum "$IMAGE" | awk '{print $1}')
        [[ $actual == $expected ]] || die "Firmware SHA-256 does not match $IMAGE.sha256"
    fi

    say "Firmware image: $IMAGE"
    sha256sum "$IMAGE"
}

flashrom_cmd() {
    if (( EUID == 0 )); then
        flashrom -p "$PROGRAMMER" -c "$CHIP" "$@"
    else
        need sudo
        sudo flashrom -p "$PROGRAMMER" -c "$CHIP" "$@"
    fi
}

backup_once() {
    local dir=$1 i file reference
    rm -rf -- "$dir"
    mkdir -p -- "$dir"
    for ((i=1; i<=BACKUP_READS; i++)); do
        file="$dir/original-nor-read-$i.bin"
        say "Reading original NOR: pass $i of $BACKUP_READS"
        flashrom_cmd -r "$file"
        [[ $(stat -c %s -- "$file") -eq $EXPECTED_SIZE ]] || {
            warn "Read $i has the wrong size"
            return 1
        }
        sha256sum "$file"
    done

    reference="$dir/original-nor-read-1.bin"
    for ((i=2; i<=BACKUP_READS; i++)); do
        cmp -s -- "$reference" "$dir/original-nor-read-$i.bin" || {
            warn 'Backup reads do not match byte-for-byte'
            return 1
        }
    done
    cp -f -- "$reference" "$dir/original-nor-confirmed.bin"
    (cd "$dir" && sha256sum ./*.bin > SHA256SUMS)
    say "All $BACKUP_READS reads match."
    say "Confirmed backup: $dir/original-nor-confirmed.bin"
}

perform_backup() {
    local dir attempt=0
    dir="$ARTIFACTS_DIR/backups/$(date +%Y%m%d-%H%M%S)"
    while :; do
        ((attempt+=1))
        say "Backup attempt $attempt"
        backup_once "$dir" && return 0
        warn 'No verified backup was produced; flashing remains blocked.'
        prompt_yes_no 'Retry the complete backup read set?' yes ||
            die 'No verified original NOR backup was produced'
    done
}

perform_flash() {
    local attempt=0 rc
    mkdir -p -- "$ARTIFACTS_DIR"
    VERIFY_FILE=$(mktemp "${TMPDIR:-/tmp}/postmerkos-flash-verify.XXXXXX.bin")
    trap '[[ -n ${VERIFY_FILE:-} ]] && rm -f -- "$VERIFY_FILE"' EXIT

    while :; do
        ((attempt+=1))
        say "Flash attempt $attempt"
        if flashrom_cmd -V -w "$IMAGE"; then
            rm -f -- "$VERIFY_FILE"
            VERIFY_FILE=$(mktemp "${TMPDIR:-/tmp}/postmerkos-flash-verify.XXXXXX.bin")
            if flashrom_cmd -r "$VERIFY_FILE" && cmp -s -- "$IMAGE" "$VERIFY_FILE"; then
                say 'Flash and explicit full-image readback verification succeeded.'
                return 0
            fi
            warn 'flashrom returned success, but the explicit readback did not match'
        else
            rc=$?
            warn "flashrom reported a write failure (status $rc)"
        fi
        prompt_yes_no 'Retry flashing?' yes || die 'Firmware was not successfully verified'
    done
}

open_serial_console() {
    local utility="$REPO_ROOT/tools/serial-console/serial-console.sh"
    [[ -x $utility ]] || { warn "Serial utility is unavailable: $utility"; return 0; }
    "$utility"
}

while (($#)); do
    case "$1" in
        --image) (($# >= 2)) || die '--image requires a file'; IMAGE=$2; shift 2 ;;
        --artifacts) (($# >= 2)) || die '--artifacts requires a directory'; ARTIFACTS_DIR=$2; shift 2 ;;
        --programmer) (($# >= 2)) || die '--programmer requires a value'; PROGRAMMER=$2; shift 2 ;;
        --chip) (($# >= 2)) || die '--chip requires a value'; CHIP=$2; shift 2 ;;
        --backup-reads) (($# >= 2)) || die '--backup-reads requires a number'; BACKUP_READS=$2; shift 2 ;;
        --backup) BACKUP_MODE=yes; shift ;;
        --no-backup) BACKUP_MODE=no; shift ;;
        --serial) OPEN_SERIAL=yes; shift ;;
        --no-serial) OPEN_SERIAL=no; shift ;;
        --help|-h) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "Unknown option: $1" ;;
        *) [[ -z $IMAGE ]] || die 'Only one firmware image may be supplied'; IMAGE=$1; shift ;;
    esac
done
(($# == 0)) || die 'Unexpected positional arguments'
[[ $BACKUP_READS =~ ^[0-9]+$ ]] && (( BACKUP_READS >= 2 )) || die '--backup-reads must be at least 2'

for command in flashrom sha256sum cmp stat dd od tr awk readlink mktemp; do need "$command"; done
resolve_image

say 'FLASHING SAFETY REQUIREMENTS'
printf '%s\n' \
    '1. Fully power off and unplug the switch before every flashrom read or write.' \
    '2. Connect the SPI programmer only while the switch is unpowered.' \
    '3. Never power the switch while the SPI programmer is electrically connected.'
pause_confirm 'Confirm that the switch is unplugged and the programmer is connected correctly.'

case $BACKUP_MODE in
    yes)
        perform_backup
        ;;
    no)
        warn 'Backup was disabled by command-line option.'
        prompt_yes_no 'Continue without a verified backup?' no || die 'Aborted before flashing'
        ;;
    ask)
        if prompt_yes_no 'Back up the original NOR before flashing?' yes; then
            perform_backup
        else
            warn 'Backup skipped by explicit user choice.'
            prompt_yes_no 'Continue without a verified backup?' no || die 'Aborted before flashing'
        fi
        ;;
    *)
        die "Invalid backup mode: $BACKUP_MODE"
        ;;
esac

pause_confirm 'The switch must still be powered off. Leave the SPI programmer connected for flashing.'
perform_flash

say 'DO NOT POWER ON THE SWITCH YET.'
say 'Disconnect the SPI programmer and every clip/wire from the switch first.'
pause_confirm 'After the programmer is fully disconnected, reconnect UART if desired, then power on the switch.'

case $OPEN_SERIAL in
    yes) open_serial_console ;;
    no) ;;
    ask) prompt_yes_no 'Open a serial console now?' yes && open_serial_console ;;
    *) die "Invalid serial mode: $OPEN_SERIAL" ;;
esac
