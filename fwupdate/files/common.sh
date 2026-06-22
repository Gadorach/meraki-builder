#!/bin/sh

FWUPDATE_STATUS_FILE=${FWUPDATE_STATUS_FILE:-/run/fwupdate/status.json}
FWUPDATE_LOG_FILE=${FWUPDATE_LOG_FILE:-/run/fwupdate/update.log}
FWUPDATE_LOCK_DIR=${FWUPDATE_LOCK_DIR:-/run/fwupdate.lock}
FWUPDATE_WORK_BASE=${FWUPDATE_WORK_BASE:-/tmp}
FWUPDATE_PLATFORM=${FWUPDATE_PLATFORM:-ms42p}
FWUPDATE_LOADER_NAME=${FWUPDATE_LOADER_NAME:-}
FWUPDATE_KERNEL_NAME=${FWUPDATE_KERNEL_NAME:-}
FWUPDATE_ROOTFS_NAME=${FWUPDATE_ROOTFS_NAME:-}
FWUPDATE_OVERLAY_NAME=${FWUPDATE_OVERLAY_NAME:-}
FWUPDATE_LOADER_SIZE=$((0x040000))
FWUPDATE_KERNEL_SIZE=$((0x2c0000))
FWUPDATE_ROOTFS_SIZE=$((0x800000))
FWUPDATE_OVERLAY_SIZE=$((0x500000))
FWUPDATE_FULL_IMAGE_SIZE=$((0x1000000))
FWUPDATE_LOADER_OFFSET=$((0x000000))
FWUPDATE_KERNEL_OFFSET=$((0x040000))
FWUPDATE_ROOTFS_OFFSET=$((0x300000))
FWUPDATE_OVERLAY_OFFSET=$((0xb00000))
FWUPDATE_SOURCES_FILE=${FWUPDATE_SOURCES_FILE:-/etc/fwupdate/sources.conf}
FWUPDATE_PRESERVE_FILE=${FWUPDATE_PRESERVE_FILE:-/etc/fwupdate/preserve.list}
FWUPDATE_HISTORY_DIR=${FWUPDATE_HISTORY_DIR:-/config/postmerkos/update-history}
FWUPDATE_RELEASE_FILE=${FWUPDATE_RELEASE_FILE:-/etc/postmerkos-release.json}
FWUPDATE_UPLOAD_DIR=${FWUPDATE_UPLOAD_DIR:-/run/fwupdate/uploads}
FWUPDATE_MANIFEST_BYTES=4096
FWUPDATE_MANIFEST_MARKER=PMOSMETA
FWUPDATE_CURRENT_VERSION=${FWUPDATE_CURRENT_VERSION:-}
FWUPDATE_TARGET_VERSION=${FWUPDATE_TARGET_VERSION:-}
FWUPDATE_MODEL=${FWUPDATE_MODEL:-}
FWUPDATE_COMPATIBILITY=${FWUPDATE_COMPATIBILITY:-untested}
FWUPDATE_LED_MODE=${FWUPDATE_LED_MODE:-unknown}
FWUPDATE_MANIFEST_HELPER=${FWUPDATE_MANIFEST_HELPER:-/usr/libexec/fwupdate/fwmanifest}
FWUPDATE_PROC_MTD=${FWUPDATE_PROC_MTD:-/proc/mtd}
FWUPDATE_FSTAB=${FWUPDATE_FSTAB:-/etc/fstab}
FWUPDATE_DEV_ROOT=${FWUPDATE_DEV_ROOT:-/dev}

mkdir -p /run/fwupdate "$FWUPDATE_UPLOAD_DIR" 2>/dev/null || true
chmod 700 "$FWUPDATE_UPLOAD_DIR" 2>/dev/null || true

ts() { date +%s 2>/dev/null || echo 0; }

json_escape() {
    # JSON-C handles quotes, backslashes, newlines and control characters.
    if [ -x "$FWUPDATE_MANIFEST_HELPER" ]; then
        "$FWUPDATE_MANIFEST_HELPER" escape "$1"
        return
    fi
    # Recovery-only fallback for an incomplete package installation.
    printf '%s' "$1" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e ':a;N;$!ba;s/\n/\\n/g' \
        -e 's/\r/\\r/g' \
        -e 's/\t/\\t/g'
}

status_write() {
    state=$1
    stage=$2
    progress=$3
    message=$4
    source=${5:-${FWUPDATE_SOURCE:-}}
    firmware=${6:-${FWUPDATE_FIRMWARE:-}}
    tmp="${FWUPDATE_STATUS_FILE}.tmp.$$"
    mkdir -p "$(dirname "$FWUPDATE_STATUS_FILE")" 2>/dev/null || true
    {
        printf '{"state":"%s","stage":"%s","progress":%s,"timestamp":%s,' \
            "$(json_escape "$state")" "$(json_escape "$stage")" "$progress" "$(ts)"
        printf '"message":"%s","source":"%s","firmware":"%s",' \
            "$(json_escape "$message")" "$(json_escape "$source")" \
            "$(json_escape "$firmware")"
        printf '"current_version":"%s","target_version":"%s",' \
            "$(json_escape "${FWUPDATE_CURRENT_VERSION:-unknown}")" \
            "$(json_escape "${FWUPDATE_TARGET_VERSION:-unknown}")"
        printf '"model":"%s","compatibility":"%s","led_mode":"%s","flash_scope":"%s"}
' \
            "$(json_escape "${FWUPDATE_MODEL:-unknown}")" \
            "$(json_escape "${FWUPDATE_COMPATIBILITY:-untested}")" \
            "$(json_escape "${FWUPDATE_LED_MODE:-unknown}")" \
            "$(json_escape "${FWUPDATE_FLASH_SCOPE:-system}")"
    } > "$tmp" && mv -f "$tmp" "$FWUPDATE_STATUS_FILE"
    printf '%s|%s|%s|%s|%s
' "$(ts)" "$state" "$stage" "$progress" "$message" \
        >> "$FWUPDATE_LOG_FILE" 2>/dev/null || true
    if [ -x /usr/libexec/fwupdate/fwstatus ]; then
        /usr/libexec/fwupdate/fwstatus "$state" "$stage" "$progress" "$message" >/dev/null 2>&1 || true
    else
        printf 'FWUPDATE|%s|%s|%s|%s
' "$state" "$stage" "$progress" "$message" > /dev/console 2>/dev/null || true
    fi
    printf 'FWUPDATE|%s|%s|%s|%s
' "$state" "$stage" "$progress" "$message"
}

fw_die() {
    msg=$1
    status_write error error 100 "$msg"
    history_failure "$msg" 2>/dev/null || true
    printf 'error: %s
' "$msg" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fw_die "required command is missing: $1"
}

file_size() {
    wc -c < "$1" | awk '{print $1}'
}

read_magic() {
    file=$1
    offset=$2
    count=$3
    dd if="$file" bs=1 skip="$offset" count="$count" 2>/dev/null
}

read_hex() {
    file=$1
    offset=$2
    count=$3
    read_magic "$file" "$offset" "$count" | hexdump -v -e '1/1 "%02x"'
}

mtd_lookup() {
    name=$1
    awk -v n="\"$name\"" '
        $4 == n {
            dev=$1; sub(/:$/, "", dev);
            print dev, $2, $3, $4;
            exit
        }
    ' "$FWUPDATE_PROC_MTD"
}

mtd_lookup_first() {
    for candidate in "$@"; do
        [ -n "$candidate" ] || continue
        entry=$(mtd_lookup "$candidate")
        if [ -n "$entry" ]; then
            printf '%s\n' "$entry"
            return 0
        fi
    done
    return 1
}

mtd_lookup_fstab_overlay() {
    dev=$(awk '$2 == "/overlay" && $1 ~ /^\/dev\/mtdblock[0-9]+$/ {print $1; exit}' "$FWUPDATE_FSTAB" 2>/dev/null)
    [ -n "$dev" ] || return 1
    num=${dev#/dev/mtdblock}
    awk -v d="mtd${num}:" '$1 == d {dev=$1; sub(/:$/, "", dev); print dev, $2, $3, $4; exit}' "$FWUPDATE_PROC_MTD"
}

mtd_lookup_unique_size() {
    expected=$1
    awk -v expected="$expected" '
        $1 ~ /^mtd[0-9]+:$/ && tolower($2) == tolower(expected) {
            count++; entry=$0
        }
        END {
            if (count == 1) {
                split(entry, fields, /[[:space:]]+/);
                dev=fields[1]; sub(/:$/, "", dev);
                print dev, fields[2], fields[3], fields[4];
            }
        }
    ' "$FWUPDATE_PROC_MTD"
}

mtd_lookup_size_named() {
    expected=$1
    shift
    for candidate in "$@"; do
        entry=$(mtd_lookup "$candidate")
        [ -n "$entry" ] || continue
        set -- $entry
        [ $((0x$2)) -eq $((0x$expected)) ] || continue
        printf '%s\n' "$entry"
        return 0
    done
    mtd_lookup_unique_size "$expected"
}

validate_mtd_entry() {
    role=$1
    expected_size=$2
    entry=$3
    [ -n "$entry" ] || fw_die "$role MTD partition was not found"
    set -- $entry
    dev=$1
    size=$((0x$2))
    erase=$((0x$3))
    label=$(printf '%s' "${4:-}" | tr -d '"')
    [ "$size" -eq "$expected_size" ] || fw_die "unexpected $role partition size: $size"
    [ "$erase" -gt 0 ] && [ $((size % erase)) -eq 0 ] || fw_die "invalid $role erase geometry: $erase"
    [ -n "$label" ] || fw_die "$role MTD label is unavailable"
    [ -c "$FWUPDATE_DEV_ROOT/$dev" ] || fw_die "$FWUPDATE_DEV_ROOT/$dev is missing"
    printf '%s|%s|%s|%s\n' "$dev" "$size" "$erase" "$label"
}

check_mtd_layout() {
    if [ -n "$FWUPDATE_ROOTFS_NAME" ]; then
        root_entry=$(mtd_lookup "$FWUPDATE_ROOTFS_NAME")
    else
        root_entry=$(mtd_lookup_first squashfs rootfs || true)
    fi
    if [ -n "$FWUPDATE_OVERLAY_NAME" ]; then
        overlay_entry=$(mtd_lookup "$FWUPDATE_OVERLAY_NAME")
    else
        overlay_entry=$(mtd_lookup_first jffs2 config overlay storage || true)
        [ -n "$overlay_entry" ] || overlay_entry=$(mtd_lookup_fstab_overlay || true)
    fi
    [ -n "$root_entry" ] || fw_die "8 MiB SquashFS MTD partition was not found"
    [ -n "$overlay_entry" ] || fw_die "5 MiB JFFS2/config MTD partition was not found"

    set -- $root_entry
    ROOT_MTD=$1
    ROOT_MTD_SIZE=$((0x$2))
    ROOT_MTD_ERASE_SIZE=$((0x$3))
    ROOT_MTD_LABEL=$(printf '%s' "${4:-}" | tr -d '"')
    set -- $overlay_entry
    OVERLAY_MTD=$1
    OVERLAY_MTD_SIZE=$((0x$2))
    OVERLAY_MTD_ERASE_SIZE=$((0x$3))
    OVERLAY_MTD_LABEL=$(printf '%s' "${4:-}" | tr -d '"')

    [ "$ROOT_MTD_SIZE" -eq "$FWUPDATE_ROOTFS_SIZE" ] || \
        fw_die "unexpected SquashFS partition size: $ROOT_MTD_SIZE"
    [ "$OVERLAY_MTD_SIZE" -eq "$FWUPDATE_OVERLAY_SIZE" ] || \
        fw_die "unexpected JFFS2/config partition size: $OVERLAY_MTD_SIZE"
    [ "$ROOT_MTD_ERASE_SIZE" -gt 0 ] && \
        [ $((ROOT_MTD_SIZE % ROOT_MTD_ERASE_SIZE)) -eq 0 ] || \
        fw_die "invalid SquashFS erase geometry: $ROOT_MTD_ERASE_SIZE"
    [ "$OVERLAY_MTD_ERASE_SIZE" -gt 0 ] && \
        [ $((OVERLAY_MTD_SIZE % OVERLAY_MTD_ERASE_SIZE)) -eq 0 ] || \
        fw_die "invalid JFFS2/config erase geometry: $OVERLAY_MTD_ERASE_SIZE"
    [ -n "$ROOT_MTD_LABEL" ] || fw_die "SquashFS MTD label is unavailable"
    [ -n "$OVERLAY_MTD_LABEL" ] || fw_die "JFFS2/config MTD label is unavailable"
    [ -c "$FWUPDATE_DEV_ROOT/$ROOT_MTD" ] || fw_die "$FWUPDATE_DEV_ROOT/$ROOT_MTD is missing"
    [ -c "$FWUPDATE_DEV_ROOT/$OVERLAY_MTD" ] || fw_die "$FWUPDATE_DEV_ROOT/$OVERLAY_MTD is missing"
    export ROOT_MTD OVERLAY_MTD ROOT_MTD_SIZE OVERLAY_MTD_SIZE \
        ROOT_MTD_ERASE_SIZE OVERLAY_MTD_ERASE_SIZE ROOT_MTD_LABEL OVERLAY_MTD_LABEL
}

check_full_mtd_layout() {
    check_mtd_layout
    if [ -n "$FWUPDATE_LOADER_NAME" ]; then
        loader_entry=$(mtd_lookup "$FWUPDATE_LOADER_NAME")
    else
        loader_entry=$(mtd_lookup_size_named 00040000 RedBoot redboot bootloader loader u-boot uboot || true)
    fi
    if [ -n "$FWUPDATE_KERNEL_NAME" ]; then
        kernel_entry=$(mtd_lookup "$FWUPDATE_KERNEL_NAME")
    else
        kernel_entry=$(mtd_lookup_size_named 002c0000 kernel linux vmlinux boot1 || true)
    fi

    loader_values=$(validate_mtd_entry bootloader "$FWUPDATE_LOADER_SIZE" "$loader_entry") || exit $?
    kernel_values=$(validate_mtd_entry kernel "$FWUPDATE_KERNEL_SIZE" "$kernel_entry") || exit $?
    IFS='|' read -r LOADER_MTD LOADER_MTD_SIZE LOADER_MTD_ERASE_SIZE LOADER_MTD_LABEL <<EOF_LOADER
$loader_values
EOF_LOADER
    IFS='|' read -r KERNEL_MTD KERNEL_MTD_SIZE KERNEL_MTD_ERASE_SIZE KERNEL_MTD_LABEL <<EOF_KERNEL
$kernel_values
EOF_KERNEL

    [ "$LOADER_MTD" != "$KERNEL_MTD" ] && [ "$LOADER_MTD" != "$ROOT_MTD" ] && \
        [ "$LOADER_MTD" != "$OVERLAY_MTD" ] && [ "$KERNEL_MTD" != "$ROOT_MTD" ] && \
        [ "$KERNEL_MTD" != "$OVERLAY_MTD" ] && [ "$ROOT_MTD" != "$OVERLAY_MTD" ] || \
        fw_die "full-flash MTD partitions are not distinct"

    export LOADER_MTD KERNEL_MTD LOADER_MTD_SIZE KERNEL_MTD_SIZE \
        LOADER_MTD_ERASE_SIZE KERNEL_MTD_ERASE_SIZE LOADER_MTD_LABEL KERNEL_MTD_LABEL
}

read_board_model() {
    model=""
    if command -v board_data >/dev/null 2>&1; then
        model=$(board_data model 2>/dev/null | tr -d '\r\n' || true)
    fi
    if [ -z "$model" ] && [ -f /run/postmerkos/boardinfo ]; then
        model=$(sed -n 's/^MODEL=//p' /run/postmerkos/boardinfo | head -n1 | tr -d '\r\n')
        [ -n "$model" ] || model=$(head -n1 /run/postmerkos/boardinfo | tr -d '\r\n')
    fi
    printf '%s' "$model"
}

model_compatibility() {
    case "$1" in
        MS42P|MS320-24P) echo validated ;;
        MS22|MS22P|MS42|MS220-*|MS320-*) echo untested ;;
        *) echo known-incompatible ;;
    esac
}

manifest_model_compatibility() {
    manifest=$1
    model=$2
    [ -x "$FWUPDATE_MANIFEST_HELPER" ] || return 1
    "$FWUPDATE_MANIFEST_HELPER" model "$manifest" "$model" 2>/dev/null
}

check_board() {
    FWUPDATE_MODEL=$(read_board_model)
    [ -n "$FWUPDATE_MODEL" ] || fw_die "could not identify the switch model"
    FWUPDATE_COMPATIBILITY=$(model_compatibility "$FWUPDATE_MODEL")
    export FWUPDATE_MODEL FWUPDATE_COMPATIBILITY
    [ "$FWUPDATE_COMPATIBILITY" != known-incompatible ] || \
        fw_die "known-incompatible board model: $FWUPDATE_MODEL"
}

json_value() {
    key=$1
    file=$2
    [ -x "$FWUPDATE_MANIFEST_HELPER" ] || return 1
    "$FWUPDATE_MANIFEST_HELPER" get "$file" "$key" 2>/dev/null
}

installed_version() {
    json_value version "$FWUPDATE_RELEASE_FILE"
}

read_candidate_manifest() {
    image=$1
    size=$(file_size "$image")
    if [ "$size" -eq "$FWUPDATE_FULL_IMAGE_SIZE" ]; then
        offset=$((FWUPDATE_ROOTFS_OFFSET + FWUPDATE_ROOTFS_SIZE - FWUPDATE_MANIFEST_BYTES))
    elif [ "$size" -eq "$FWUPDATE_ROOTFS_SIZE" ]; then
        offset=$((FWUPDATE_ROOTFS_SIZE - FWUPDATE_MANIFEST_BYTES))
    else
        return 1
    fi
    marker=$(dd if="$image" bs=1 skip="$offset" count=8 2>/dev/null)
    [ "$marker" = "$FWUPDATE_MANIFEST_MARKER" ] || return 1
    lenhex=$(dd if="$image" bs=1 skip=$((offset + 8)) count=8 2>/dev/null)
    case "$lenhex" in ''|*[!0-9A-Fa-f]*) return 1 ;; esac
    length=$((0x$lenhex))
    [ "$length" -gt 0 ] && [ "$length" -le $((FWUPDATE_MANIFEST_BYTES - 16)) ] || return 1
    CANDIDATE_MANIFEST=/run/fwupdate/candidate-manifest.$$.json
    dd if="$image" of="$CANDIDATE_MANIFEST" bs=1 skip=$((offset + 16)) count="$length" 2>/dev/null || return 1
    "$FWUPDATE_MANIFEST_HELPER" validate "$CANDIDATE_MANIFEST" >/dev/null 2>&1 || return 1
    FWUPDATE_TARGET_VERSION=$(json_value version "$CANDIDATE_MANIFEST")
    [ -n "$FWUPDATE_TARGET_VERSION" ] || return 1
    export CANDIDATE_MANIFEST FWUPDATE_TARGET_VERSION
    return 0
}

history_prepare() {
    mkdir -p "$FWUPDATE_HISTORY_DIR" 2>/dev/null || return 0
    cat > "$FWUPDATE_HISTORY_DIR/pending.json.tmp" <<EOF_HISTORY
{"state":"pending","started":$(ts),"current_version":"$(json_escape "${FWUPDATE_CURRENT_VERSION:-unknown}")","target_version":"$(json_escape "${FWUPDATE_TARGET_VERSION:-unknown}")","model":"$(json_escape "${FWUPDATE_MODEL:-unknown}")","compatibility":"$(json_escape "${FWUPDATE_COMPATIBILITY:-untested}")","source":"$(json_escape "${FWUPDATE_SOURCE:-unknown}")","firmware":"$(json_escape "${FWUPDATE_FIRMWARE:-unknown}")","sha256":"$(json_escape "${FWUPDATE_SHA256:-unknown}")","overlay":"$(json_escape "${OVERLAY_POLICY:-preserve}")","flash_scope":"$(json_escape "${FWUPDATE_FLASH_SCOPE:-system}")"}
EOF_HISTORY
    mv -f "$FWUPDATE_HISTORY_DIR/pending.json.tmp" "$FWUPDATE_HISTORY_DIR/pending.json"
    cp -f "$FWUPDATE_LOG_FILE" "$FWUPDATE_HISTORY_DIR/pending.log" 2>/dev/null || true
    sync
}

history_failure() {
    mkdir -p "$FWUPDATE_HISTORY_DIR" 2>/dev/null || return 0
    cat > "$FWUPDATE_HISTORY_DIR/last.json.tmp" <<EOF_HISTORY
{"result":"failed","completed":$(ts),"current_version":"$(json_escape "${FWUPDATE_CURRENT_VERSION:-unknown}")","target_version":"$(json_escape "${FWUPDATE_TARGET_VERSION:-unknown}")","model":"$(json_escape "${FWUPDATE_MODEL:-unknown}")","message":"$(json_escape "$1")"}
EOF_HISTORY
    mv -f "$FWUPDATE_HISTORY_DIR/last.json.tmp" "$FWUPDATE_HISTORY_DIR/last.json"
    cp -f "$FWUPDATE_LOG_FILE" "$FWUPDATE_HISTORY_DIR/last.log" 2>/dev/null || true
}

led_detect() {
    command -v postmerkos-hwprobe >/dev/null 2>&1 && postmerkos-hwprobe refresh >/dev/null 2>&1 || true
    if [ -r /run/postmerkos/led-capabilities.env ]; then
        . /run/postmerkos/led-capabilities.env
    fi
    if [ "${PORT_LED_AVAILABLE:-0}" = 1 ] && [ "${PORT_LED_VERIFIED:-0}" = 1 ]; then
        FWUPDATE_LED_MODE=binary-poe-ports
    elif [ "${STATUS_LED_AVAILABLE:-0}" = 1 ] && [ "${STATUS_LED_VERIFIED:-0}" = 1 ]; then
        FWUPDATE_LED_MODE=binary-status-led
    else
        FWUPDATE_LED_MODE=unavailable
    fi
    export FWUPDATE_LED_MODE
}


check_work_base_ram() {
    case "$FWUPDATE_WORK_BASE" in
        /tmp|/tmp/*) mount_at=/tmp ;;
        /run|/run/*) mount_at=/run ;;
        *) fw_die "firmware work directory must be under /tmp or /run" ;;
    esac
    fs_type=$(awk -v m="$mount_at" '$2 == m {print $3; exit}' /proc/mounts 2>/dev/null)
    [ "$fs_type" = tmpfs ] ||         fw_die "$mount_at must be a tmpfs before firmware updating is allowed"
}

valid_sha256() {
    printf '%s' "$1" | grep -Eq '^[0-9a-fA-F]{64}$'
}

verify_sidecar() {
    image=$1
    sidecar=$2
    [ -f "$sidecar" ] || fw_die "checksum sidecar not found: $sidecar"
    hash=$(awk 'NF >= 2 && $1 !~ /^#/ {print tolower($1); exit}' "$sidecar")
    listed=$(awk 'NF >= 2 && $1 !~ /^#/ {$1=""; sub(/^[ \t]+[*]?/, ""); sub(/\r$/, ""); print; exit}' "$sidecar")
    valid_sha256 "$hash" || fw_die "invalid SHA-256 in $sidecar"
    expected_name=$(basename "$image")
    [ "$listed" = "$expected_name" ] || \
        fw_die "checksum filename '$listed' does not match '$expected_name'"
    actual=$(sha256sum "$image" | awk '{print $1}')
    [ "$actual" = "$hash" ] || fw_die "SHA-256 mismatch for $expected_name"
    FWUPDATE_SHA256=$actual
    export FWUPDATE_SHA256
}

verify_auxiliary_sidecar() {
    file=$1
    sidecar=$2
    [ -f "$file" ] && [ -f "$sidecar" ] || return 1
    hash=$(awk 'NF >= 2 && $1 !~ /^#/ {print tolower($1); exit}' "$sidecar")
    listed=$(awk 'NF >= 2 && $1 !~ /^#/ {$1=""; sub(/^[ \t]+[*]?/, ""); sub(/\r$/, ""); print; exit}' "$sidecar")
    valid_sha256 "$hash" || return 1
    [ "$listed" = "$(basename "$file")" ] || return 1
    [ "$(sha256sum "$file" | awk '{print $1}')" = "$hash" ]
}

verify_expected_hash() {
    image=$1
    expected=$(printf '%s' "$2" | tr 'A-F' 'a-f')
    valid_sha256 "$expected" || fw_die "invalid expected SHA-256"
    actual=$(sha256sum "$image" | awk '{print $1}')
    [ "$actual" = "$expected" ] || fw_die "SHA-256 mismatch for $(basename "$image")"
    FWUPDATE_SHA256=$actual
    export FWUPDATE_SHA256
}

safe_remote_filename() {
    case "$1" in
        ""|*/*|*..*|*[!A-Za-z0-9._-]*) return 1 ;;
        *) return 0 ;;
    esac
}

safe_remote_path() {
    case "$1" in
        ""|*..*|*[!A-Za-z0-9._/-]*) return 1 ;;
        *) return 0 ;;
    esac
}

try_fetch_http_file() {
    url=$1
    out=$2
    rm -f "$out.part"
    case "$url" in
        https://*)
            command -v curl >/dev/null 2>&1 || return 2
            set -- curl --fail --location --show-error --silent \
                --proto '=https' --proto-redir '=https' \
                --retry 3 --connect-timeout 15 --max-time 900
            [ -f /etc/ssl/certs/ca-certificates.crt ] && \
                set -- "$@" --cacert /etc/ssl/certs/ca-certificates.crt
            set -- "$@" --output "$out.part" "$url"
            "$@" || { rm -f "$out.part"; return 1; }
            ;;
        http://*)
            if command -v curl >/dev/null 2>&1; then
                curl --fail --location --show-error --silent \
                    --proto '=http,https' --proto-redir '=http,https' \
                    --retry 3 --connect-timeout 15 --max-time 900 \
                    --output "$out.part" "$url" || { rm -f "$out.part"; return 1; }
            else
                wget -T 30 -t 3 -O "$out.part" "$url" || { rm -f "$out.part"; return 1; }
            fi
            ;;
        *) return 3 ;;
    esac
    mv -f "$out.part" "$out"
}

fetch_http_file() {
    url=$1
    out=$2
    try_fetch_http_file "$url" "$out"
    rc=$?
    [ "$rc" -eq 0 ] && return 0
    case "$rc" in
        2) fw_die "HTTPS requires curl support in the firmware image" ;;
        3) fw_die "unsupported HTTP source URL: $url" ;;
        *) fw_die "download failed: $url" ;;
    esac
}

acquire_lock() {
    if mkdir "$FWUPDATE_LOCK_DIR" 2>/dev/null; then
        echo $$ > "$FWUPDATE_LOCK_DIR/pid"
        return 0
    fi

    owner=$(cat "$FWUPDATE_LOCK_DIR/pid" 2>/dev/null || true)
    [ "$owner" = "$$" ] && return 0
    case "$owner" in
        ''|*[!0-9]*) ;;
        *)
            if kill -0 "$owner" 2>/dev/null; then
                printf 'error: another firmware update appears to be active (pid %s)\n' "$owner" >&2
                exit 1
            fi
            ;;
    esac

    rm -rf "$FWUPDATE_LOCK_DIR" 2>/dev/null || true
    mkdir "$FWUPDATE_LOCK_DIR" 2>/dev/null || {
        printf 'error: another firmware update appears to be active\n' >&2
        exit 1
    }
    echo $$ > "$FWUPDATE_LOCK_DIR/pid"
}

release_lock() {
    owner=$(cat "$FWUPDATE_LOCK_DIR/pid" 2>/dev/null || true)
    if [ -z "$owner" ] || [ "$owner" = "$$" ]; then
        rm -rf "$FWUPDATE_LOCK_DIR" 2>/dev/null || true
    fi
}

cleanup_transport_work() {
    path=${FWUPDATE_TRANSPORT_WORK:-}
    case "$path" in
        "$FWUPDATE_WORK_BASE"/fwupdate-http.*|\
        "$FWUPDATE_WORK_BASE"/fwupdate-tftp.*|\
        "$FWUPDATE_WORK_BASE"/fwupdate-sftp.*)
            rm -rf "$path" 2>/dev/null || true
            ;;
    esac
    FWUPDATE_TRANSPORT_WORK=
    export FWUPDATE_TRANSPORT_WORK
}
