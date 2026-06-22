#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
LOG_DIR=${MERAKI_SERIAL_LOG_DIR:-"$REPO_ROOT/logs"}
BAUD=${BAUD:-115200}
DEVICE=""
ENABLE_LOG=1

usage() {
    cat <<'USAGE'
usage: serial-console.sh [options] [DEVICE]

Open a postmerkOS hardware console with the firmware's expected serial settings:
115200 baud, 8 data bits, no parity, 1 stop bit, and XON/XOFF flow control.

Options:
  --device PATH     serial device, such as /dev/ttyUSB0
  --baud RATE       override the default 115200 baud rate
  --log-dir DIR     log directory (default: ../../logs)
  --no-log          do not save a transcript
  --list            list detected serial devices and exit
  --help            show this help

Current firmware may show `pmc:` or the postmerkOS management menu instead of a
shell. Type `shell` at either prompt when the logged-in role permits raw-shell
access.
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

list_serial_ports() {
    local path resolved
    for path in /dev/serial/by-id/* /dev/ttyUSB* /dev/ttyACM*; do
        [[ -e $path ]] || continue
        resolved=$(readlink -f -- "$path" 2>/dev/null || printf '%s' "$path")
        printf '%s\t%s\n' "$path" "$resolved"
    done | awk -F '\t' '!seen[$2]++ { print $1 }'
}

device_group() {
    local resolved
    resolved=$(readlink -f -- "$1" 2>/dev/null || printf '%s' "$1")
    stat -c '%G' "$resolved" 2>/dev/null || true
}

has_direct_access() {
    local resolved
    resolved=$(readlink -f -- "$1" 2>/dev/null || printf '%s' "$1")
    [[ -r $resolved && -w $resolved ]]
}

run_privileged() {
    if (( EUID == 0 )); then
        "$@"
    else
        need sudo
        sudo "$@"
    fi
}

choose_access_mode() {
    local port=$1 owner_group current_user choice
    has_direct_access "$port" && { printf 'direct'; return; }

    current_user=$(id -un)
    owner_group=$(device_group "$port")
    warn "The current session cannot read and write $port"
    [[ -n $owner_group ]] && printf 'Device group: %s\n' "$owner_group" >&2

    while :; do
        cat >&2 <<'MENU'
Choose how to continue:
  1) Open picocom with sudo for this run
  2) Add the current user to the device group, then use sudo for this run
  3) Select another serial device
  0) Exit
MENU
        read -r -p 'Selection [1]: ' choice
        choice=${choice:-1}
        case $choice in
            1) printf 'sudo'; return ;;
            2)
                [[ -n $owner_group && $owner_group != UNKNOWN ]] || {
                    warn 'The serial-device group could not be determined'
                    continue
                }
                if id -nG "$current_user" | tr ' ' '\n' | grep -Fxq "$owner_group"; then
                    printf '\nUser %q already belongs to %q; this login session has not gained access yet.\n' \
                        "$current_user" "$owner_group" >&2
                else
                    run_privileged usermod -aG "$owner_group" "$current_user"
                    printf '\nAdded %q to %q. Log out and back in before direct access will work.\n' \
                        "$current_user" "$owner_group" >&2
                fi
                printf 'sudo'
                return
                ;;
            3) printf 'reselect'; return ;;
            0) printf 'exit'; return ;;
            *) warn 'Invalid selection' ;;
        esac
    done
}

run_console() {
    local port=$1 mode=$2 log_file="" rc
    if (( ENABLE_LOG )); then
        mkdir -p -- "$LOG_DIR"
        log_file="$LOG_DIR/serial-$(date +%Y%m%d-%H%M%S).log"
    fi

    say "Opening $port at $BAUD baud, 8N1, XON/XOFF."
    say 'Exit picocom with Ctrl+A, then Ctrl+X.'
    [[ -n $log_file ]] && say "Serial log: $log_file"

    set +e
    if [[ $mode == sudo ]]; then
        if [[ -n $log_file ]]; then
            run_privileged picocom -b "$BAUD" -d 8 -p n -f s "$port" 2>&1 | tee "$log_file"
            rc=${PIPESTATUS[0]}
        else
            run_privileged picocom -b "$BAUD" -d 8 -p n -f s "$port"
            rc=$?
        fi
    elif [[ -n $log_file ]]; then
        picocom -b "$BAUD" -d 8 -p n -f s "$port" 2>&1 | tee "$log_file"
        rc=${PIPESTATUS[0]}
    else
        picocom -b "$BAUD" -d 8 -p n -f s "$port"
        rc=$?
    fi
    set -e
    return "$rc"
}

select_device() {
    local ports=() choice index
    if [[ -n $DEVICE ]]; then
        [[ -c $DEVICE ]] || die "Serial device is not a character device: $DEVICE"
        return
    fi

    mapfile -t ports < <(list_serial_ports)
    ((${#ports[@]})) || die 'No /dev/ttyUSB*, /dev/ttyACM*, or /dev/serial/by-id device was found'
    say 'Available serial interfaces:'
    for index in "${!ports[@]}"; do
        printf '  %d) %s\n' "$((index + 1))" "${ports[$index]}"
    done
    printf '  m) Enter a device manually\n  0) Exit\n'

    while :; do
        read -r -p 'Select a serial interface: ' choice
        case $choice in
            0) exit 0 ;;
            m|M)
                read -r -p 'Serial device path: ' DEVICE
                [[ -c $DEVICE ]] || { warn 'Not a character device'; DEVICE=""; continue; }
                return
                ;;
        esac
        [[ $choice =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#ports[@]} )) || {
            warn 'Invalid selection'
            continue
        }
        DEVICE=${ports[$((choice - 1))]}
        return
    done
}

LIST_ONLY=0
while (($#)); do
    case $1 in
        --device) (($# >= 2)) || die '--device requires a path'; DEVICE=$2; shift 2 ;;
        --baud) (($# >= 2)) || die '--baud requires a rate'; BAUD=$2; shift 2 ;;
        --log-dir) (($# >= 2)) || die '--log-dir requires a directory'; LOG_DIR=$2; shift 2 ;;
        --no-log) ENABLE_LOG=0; shift ;;
        --list) LIST_ONLY=1; shift ;;
        --help|-h) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "Unknown option: $1" ;;
        *) [[ -z $DEVICE ]] || die 'Only one serial device may be supplied'; DEVICE=$1; shift ;;
    esac
done
(($# == 0)) || die 'Unexpected positional arguments'
[[ $BAUD =~ ^[0-9]+$ ]] && (( BAUD > 0 )) || die '--baud must be a positive integer'

need awk
need readlink
if (( LIST_ONLY )); then
    list_serial_ports
    exit 0
fi
need grep
need id
need picocom
need stat
need tr

while :; do
    select_device
    mode=$(choose_access_mode "$DEVICE")
    case $mode in
        direct|sudo)
            if run_console "$DEVICE" "$mode"; then
                exit 0
            fi
            rc=$?
            warn "Serial connection exited with status $rc"
            prompt_yes_no 'Select a serial device and retry?' yes || exit "$rc"
            DEVICE=""
            ;;
        reselect) DEVICE="" ;;
        exit) exit 0 ;;
        *) die "Unexpected access mode: $mode" ;;
    esac
done
