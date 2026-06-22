#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
load_build_state
need python3
need unsquashfs
need file
need readelf
need strings

IMAGE="${1:-}"
if [[ -z "$IMAGE" && -f "$ARTIFACTS_DIR/latest-image.txt" ]]; then
  IMAGE="$(cat "$ARTIFACTS_DIR/latest-image.txt")"
fi
[[ -f "$IMAGE" ]] || die "No firmware image was supplied and latest-image.txt is unavailable"
[[ "$(file_size "$IMAGE")" -eq $((0x1000000)) ]] || die "Firmware image is not exactly 16 MiB"

python3 "$SCRIPT_DIR/validate-vcoreiii-payload.py" "$IMAGE" \
  --expected-payload "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" \
  --json-output "$ARTIFACTS_DIR/kernel-payload-validation.json"
python3 - "$IMAGE" <<'PY'
from pathlib import Path
import sys
image = Path(sys.argv[1]).read_bytes()
assert image[0x300000:0x300004] == b'hsqs', 'missing SquashFS magic at 0x300000'
PY

VERIFY_DIR="$BUILD_DIR/rootfs-validation"
rm -rf "$VERIFY_DIR"
unsquashfs -quiet -d "$VERIFY_DIR" "$ARTIFACTS_DIR/rootfs.squashfs"

required=(
  etc/fstab
  etc/init.d/S08kmods
  etc/init.d/S09clickinit
  etc/init.d/S10clickconfig
  etc/init.d/S11poe
  lib/modules/postmerkos-required-modules.txt
  lib/modules/postmerkos-all-modules.txt
  lib/modules/postmerkos-modules.sha256
  bin/pd690xx
  bin/fw_update
  bin/fw_update_http
  bin/fw_update_tftp
  bin/fw_update_sftp
  bin/fw_update_status
  usr/lib/fwupdate/common.sh
  usr/libexec/fwupdate/fwflash
  etc/fwupdate/sources.conf
  etc/fwupdate/preserve.list
  bin/configd
  usr/bin/postmerkosctl
  etc/init.d/S15configd
  usr/sbin/postmerkos-configd-supervisor
)
if bool_enabled "${INCLUDE_UI:-0}"; then
  required+=(www/index.html usr/bin/uhttpd etc/init.d/S16uhttpd \
    usr/sbin/postmerkos-network-rebind etc/postmerkos/features/web-ui)
fi
for path in "${required[@]}"; do
  [[ -e "$VERIFY_DIR/$path" ]] || die "Rootfs verification failed: missing /$path"
done
python3 "$VENDOR_MODULE_TOOL" verify "$VERIFY_DIR/lib/modules" \
  --required-file "$VENDOR_MODULE_REQUIRED" --quiet || \
  die "Rootfs verification failed: platform-complete vendor module matrix is missing"


if bool_enabled "${INCLUDE_UI:-0}"; then
  for path in bin/configd usr/bin/uhttpd etc/init.d/S15configd \
    usr/sbin/postmerkos-configd-supervisor \
      etc/init.d/S16uhttpd usr/sbin/postmerkos-network-rebind; do
    [[ -x "$VERIFY_DIR/$path" ]] || \
      die "Rootfs verification failed: /$path is not executable"
  done

  grep -R -a -q 'configd-ws' "$VERIFY_DIR/www" || die "Web UI lacks configd-ws protocol marker"
  grep -R -a -q '4001' "$VERIFY_DIR/www" || die "Web UI lacks configd WebSocket port 4001"
  grep -Fq 'management-health --quiet' "$VERIFY_DIR/etc/init.d/S15configd" || \
    die "configd init lacks the WebSocket hello health contract"
  grep -Fq 'postmerkOS management: restarting configd' \
    "$VERIFY_DIR/usr/sbin/postmerkos-configd-supervisor" || \
    die "configd supervisor restart contract is missing"
  strings "$VERIFY_DIR/usr/bin/postmerkosctl" | grep -F 'WebSocket configd-ws hello' >/dev/null || \
    die "postmerkosctl lacks the WebSocket hello health probe"
  strings "$VERIFY_DIR/bin/configd" | grep -Fx 'websocket: enabled' >/dev/null || {
    detected_features="$(strings "$VERIFY_DIR/bin/configd" | grep -E '^websocket: (enabled|disabled)$' || true)"
    die "Web image configd feature marker mismatch; found: ${detected_features:-none}"
  }
  readelf -d "$VERIFY_DIR/bin/configd" | grep -F 'libwebsockets' >/dev/null || \
    die "Web image configd is not linked against libwebsockets"
  grep -Fq 'web image contains WebSocket-disabled configd' \
    "$VERIFY_DIR/etc/init.d/S15configd" || \
    die "configd init does not fail closed for a WebSocket-disabled web image"
fi

file "$VERIFY_DIR/usr/libexec/fwupdate/fwflash" | grep -i 'statically linked' >/dev/null || \
  die "Firmware updater helper is not statically linked"

rootfs_has_command() {
  local command="$1" dir
  for dir in bin sbin usr/bin usr/sbin; do
    [[ -x "$VERIFY_DIR/$dir/$command" ]] && return 0
  done
  return 1
}

for command in curl mkfs.jffs2 hexdump sha256sum fuser mountpoint head dd awk sed killall umount; do
  rootfs_has_command "$command" || die "Firmware updater dependency is missing: $command"
done

if ! bool_enabled "${INCLUDE_UI:-0}"; then
  [[ ! -e "$VERIFY_DIR/etc/init.d/S16uhttpd" ]] || die "Base image unexpectedly contains S16uhttpd"
  [[ ! -e "$VERIFY_DIR/etc/postmerkos/features/web-ui" ]] || die "Base image unexpectedly requires WebSocket service"
  strings "$VERIFY_DIR/bin/configd" | grep -Fx 'websocket: disabled' >/dev/null || \
    die "Base image configd unexpectedly includes WebSocket support"
fi

find "$VERIFY_DIR/etc/init.d" -maxdepth 1 -type f -printf '%f\n' | sort \
  > "$ARTIFACTS_DIR/effective-init-scripts.txt"
find "$VERIFY_DIR/lib/modules" -type f -printf '%P\n' | sort \
  > "$ARTIFACTS_DIR/effective-module-files.txt"
write_sha256_sidecar "$IMAGE"
touch "$STAMP_DIR/image-validated"
log "Validated $IMAGE"
