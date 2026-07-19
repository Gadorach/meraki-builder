#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/overlay/etc/init.d/S01postmerkos-overlay"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/bin" "$TMP/run" "$TMP/overlay" "$TMP/etc" "$TMP/root"
: >"$TMP/console"
: >"$TMP/mounts"
: >"$TMP/cmdline"

cat >"$TMP/bin/mount" <<'MOCK'
#!/bin/sh
set -eu
mounts=${POSTMERKOS_PROC_MOUNTS:?}
if [ "$#" -eq 1 ]; then
  target=$1
  if [ "$target" = "$POSTMERKOS_OVERLAY_MOUNT" ]; then
    [ "${MOCK_PERSISTENT_FAIL:-0}" = 1 ] && exit 1
    printf '/dev/mtdblock3 %s jffs2 rw 0 0\n' "$target" >>"$mounts"
    exit 0
  fi
  exit 1
fi
if [ "$1" = -t ] && [ "$2" = tmpfs ]; then
  target=${6}
  printf 'postmerkos-recovery %s tmpfs rw 0 0\n' "$target" >>"$mounts"
  exit 0
fi
if [ "$1" = -t ] && [ "$2" = overlay ]; then
  target=${6}
  printf 'overlay %s overlay rw 0 0\n' "$target" >>"$mounts"
  exit 0
fi
exit 1
MOCK
cat >"$TMP/bin/umount" <<'MOCK'
#!/bin/sh
set -eu
target=$1
awk -v target="$target" '$2 != target' "$POSTMERKOS_PROC_MOUNTS" >"$POSTMERKOS_PROC_MOUNTS.tmp"
mv "$POSTMERKOS_PROC_MOUNTS.tmp" "$POSTMERKOS_PROC_MOUNTS"
MOCK
chmod +x "$TMP/bin/mount" "$TMP/bin/umount"

run_init() {
  POSTMERKOS_PATH="$TMP/bin:$PATH" \
  POSTMERKOS_RUN_DIR="$TMP/run" \
  POSTMERKOS_OVERLAY_MOUNT="$TMP/overlay" \
  POSTMERKOS_ETC_TARGET="$TMP/etc" \
  POSTMERKOS_ROOT_TARGET="$TMP/root" \
  POSTMERKOS_PROC_MOUNTS="$TMP/mounts" \
  POSTMERKOS_PROC_CMDLINE="$TMP/cmdline" \
  POSTMERKOS_OVERLAY_RECOVERY_MARKER="$TMP/run/recovery.json" \
  POSTMERKOS_LIVE_MARKER="$TMP/run/live.json" \
  POSTMERKOS_OVERLAY_LOG="$TMP/run/overlay.log" \
  POSTMERKOS_CONSOLE="$TMP/console" \
  MOCK_PERSISTENT_FAIL="${1:-0}" "$SCRIPT" start
}

run_init 0
for path in .upper/etc .work/etc .upper/root .work/root; do
  [ -d "$TMP/overlay/$path" ]
done
grep -Fq "$TMP/overlay jffs2" "$TMP/mounts"
grep -Fq "$TMP/etc overlay" "$TMP/mounts"
grep -Fq "$TMP/root overlay" "$TMP/mounts"
[ ! -e "$TMP/run/recovery.json" ]

# A missing persistent mount must keep the management overlays writable using
# tmpfs and publish a prominent non-persistence marker. Simulate mount -a having
# left partial child mounts behind; recovery must detach those before replacing
# /overlay or the fallback would fail with EBUSY on real hardware.
printf 'overlay %s overlay rw 0 0\noverlay %s overlay rw 0 0\n' \
  "$TMP/etc" "$TMP/root" >"$TMP/mounts"
rm -rf "$TMP/overlay"/* "$TMP/run/recovery.json"
run_init 1
grep -Fq "$TMP/overlay tmpfs" "$TMP/mounts"
grep -q '"persistence":false' "$TMP/run/recovery.json"
grep -Fq "$TMP/etc overlay" "$TMP/mounts"
grep -Fq "$TMP/root overlay" "$TMP/mounts"


# PMOSLIVE must never attempt the JFFS2 mount. It must create an explicit live
# marker and use the same tmpfs-backed overlay machinery for /etc and /root.
: >"$TMP/mounts"
rm -rf "$TMP/overlay"/* "$TMP/run/recovery.json" "$TMP/run/live.json"
printf '%s\n' 'console=ttyS0,115200 postmerkos.live=1 root=/dev/ram0' >"$TMP/cmdline"
run_init 0
! grep -Fq ' jffs2 ' "$TMP/mounts"
grep -Fq "$TMP/overlay tmpfs" "$TMP/mounts"
grep -q '"transport":"pmoslive"' "$TMP/run/live.json"
grep -q '"flash_mounted":false' "$TMP/run/live.json"
grep -q '"persistence":false' "$TMP/run/recovery.json"
grep -Fq "$TMP/etc overlay" "$TMP/mounts"
grep -Fq "$TMP/root overlay" "$TMP/mounts"
grep -Fq 'PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0' "$TMP/console"

printf '%s\n' 'early overlay recovery and PMOSLIVE init tests passed'
