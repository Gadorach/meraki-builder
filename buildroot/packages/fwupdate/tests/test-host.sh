#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PKG=$(CDPATH= cd -- "$HERE/.." && pwd)
OUT=${TMPDIR:-/tmp}/fwmanifest-test

cc -std=c99 -Wall -Wextra -Werror \
  $(pkg-config --cflags json-c) \
  -o "$OUT" "$PKG/fwmanifest.c" $(pkg-config --libs json-c)

[ "$("$OUT" compare 1.10.0 1.9.9)" = 1 ]
[ "$("$OUT" compare 1.0.0-rc2 1.0.0-rc10)" = -1 ]
[ "$("$OUT" compare 2026.06.18 2026.06.18)" = 0 ]

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
cat >"$TMP/manifest.json" <<'JSON'
{
  "version": "2026.06.18",
  "artifact": {
    "filename": "switch.bin",
    "bytes": 16777216,
    "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  },
  "models": {
    "MS42P": "validated",
    "MS220-8": "untested"
  }
}
JSON
"$OUT" validate "$TMP/manifest.json"
[ "$("$OUT" get "$TMP/manifest.json" version)" = 2026.06.18 ]
[ "$("$OUT" get "$TMP/manifest.json" artifact.bytes)" = 16777216 ]
[ "$("$OUT" model "$TMP/manifest.json" MS42P)" = validated ]
escaped=$("$OUT" escape 'quote=" slash=\ newline
next')
printf '{"value":"%s"}\n' "$escaped" | python3 -m json.tool >/dev/null

mkdir -p "$TMP/dev"
cat >"$TMP/proc-mtd" <<'MTD'
dev:    size   erasesize  name
mtd0: 00040000 00010000 "RedBoot"
mtd1: 002c0000 00010000 "kernel"
mtd2: 00800000 00010000 "squashfs"
mtd3: 00500000 00010000 "jffs2"
MTD
: >"$TMP/fstab"
for node in 0 1 2 3; do mknod "$TMP/dev/mtd$node" c 1 3; done
FWUPDATE_PROC_MTD="$TMP/proc-mtd" \
FWUPDATE_FSTAB="$TMP/fstab" \
FWUPDATE_DEV_ROOT="$TMP/dev" \
FWUPDATE_MANIFEST_HELPER="$OUT" \
FWUPDATE_STATUS_FILE="$TMP/status.json" \
FWUPDATE_LOG_FILE="$TMP/update.log" \
FWUPDATE_UPLOAD_DIR="$TMP/uploads" \
sh -c '. "$1"; check_mtd_layout; [ "$ROOT_MTD" = mtd2 ]; [ "$ROOT_MTD_SIZE" -eq 8388608 ]; [ "$ROOT_MTD_ERASE_SIZE" -eq 65536 ]; [ "$OVERLAY_MTD" = mtd3 ]; [ "$OVERLAY_MTD_SIZE" -eq 5242880 ]; check_full_mtd_layout; [ "$LOADER_MTD" = mtd0 ]; [ "$LOADER_MTD_SIZE" -eq 262144 ]; [ "$KERNEL_MTD" = mtd1 ]; [ "$KERNEL_MTD_SIZE" -eq 2883584 ]' sh "$PKG/files/common.sh"

printf 'console=ttyS0 postmerkos.live=1 root=/dev/ram0\n' > "$TMP/cmdline-live"
printf 'console=ttyS0 root=/dev/mtdblock2\n' > "$TMP/cmdline-flash"
FWUPDATE_PROC_CMDLINE="$TMP/cmdline-live" FWUPDATE_LIVE_MARKER="$TMP/no-marker" \
FWUPDATE_MANIFEST_HELPER="$OUT" FWUPDATE_STATUS_FILE="$TMP/status.json" \
FWUPDATE_LOG_FILE="$TMP/update.log" FWUPDATE_UPLOAD_DIR="$TMP/uploads" \
sh -c '. "$1"; is_live_boot' sh "$PKG/files/common.sh"
if FWUPDATE_PROC_CMDLINE="$TMP/cmdline-flash" FWUPDATE_LIVE_MARKER="$TMP/no-marker" \
  FWUPDATE_MANIFEST_HELPER="$OUT" FWUPDATE_STATUS_FILE="$TMP/status.json" \
  FWUPDATE_LOG_FILE="$TMP/update.log" FWUPDATE_UPLOAD_DIR="$TMP/uploads" \
  sh -c '. "$1"; is_live_boot' sh "$PKG/files/common.sh"; then
  echo 'normal flash boot was incorrectly detected as PMOSLIVE' >&2
  exit 1
fi

cc -std=c99 -Wall -Wextra -Werror -o "$TMP/fwflash" "$PKG/fwflash.c"
"$TMP/fwflash" --help | grep -q -- '--factory-reset'
"$TMP/fwflash" --help | grep -q -- '--overlay-only'

# The factory-default JFFS2 seed must contain the upper/work layout required by
# the /etc and /root overlayfs mounts before the first boot.
mkdir -p "$TMP/fake-bin"
cat >"$TMP/fake-bin/mkfs.jffs2" <<'EOF_MKFS'
#!/bin/sh
out=
root=
while [ "$#" -gt 0 ]; do
  case "$1" in -o) out=$2; shift 2;; -r) root=$2; shift 2;; *) shift;; esac
done
[ -d "$root/.upper/etc" ] && [ -d "$root/.work/etc" ] &&
[ -d "$root/.upper/root" ] && [ -d "$root/.work/root" ] || exit 9
python3 - "$out" <<'PY_IMAGE'
from pathlib import Path
import sys
p=Path(sys.argv[1]); p.write_bytes(bytes.fromhex('8519') + b'\xff' * (5*1024*1024-2))
PY_IMAGE
EOF_MKFS
chmod +x "$TMP/fake-bin/mkfs.jffs2"
PATH="$TMP/fake-bin:$PATH" FWUPDATE_MANIFEST_HELPER="$OUT" \
FWUPDATE_STATUS_FILE="$TMP/status.json" FWUPDATE_LOG_FILE="$TMP/update.log" \
FWUPDATE_UPLOAD_DIR="$TMP/uploads" sh -c '. "$1"; build_clean_overlay_image "$2/tree" "$2/defaults.jffs2"; [ "$(file_size "$2/defaults.jffs2")" -eq "$FWUPDATE_OVERLAY_SIZE" ]' sh "$PKG/files/common.sh" "$TMP"

cat >"$TMP/test-fwflash-led.c" <<EOF
#define main fwflash_program_main
#include "$PKG/fwflash.c"
#undef main
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    status_led_active = 1;
    status_led_protocol = STATUS_LED_PLAIN_BOOL;
    write_led_path(argv[1], 1);
    write_led_path(argv[2], 0);
    FILE *status = fopen(status_path, "w");
    if (!status) return 3;
    fputs("{\"state\":\"rollback\",\"progress\":73}\n", status);
    fclose(status);
    int progress = 0;
    bool error = false;
    read_animation_state(&progress, &error);
    return progress == 73 && error ? 0 : 4;
}
EOF
cc -std=c99 -Wall -Wextra -Werror -o "$TMP/test-fwflash-led" "$TMP/test-fwflash-led.c"
: > "$TMP/green"
: > "$TMP/orange"
# Override the compiled default status path without affecting production source.
sed -i "s#static const char \*status_path = DEFAULT_STATUS;#static const char *status_path = \"$TMP/flash-status.json\";#" "$TMP/test-fwflash-led.c"
cc -std=c99 -Wall -Wextra -Werror -o "$TMP/test-fwflash-led" "$TMP/test-fwflash-led.c"
"$TMP/test-fwflash-led" "$TMP/green" "$TMP/orange"
grep -qx '1' "$TMP/green"
grep -qx '0' "$TMP/orange"
! grep -q 'STATE' "$TMP/green"

# The updater must prefer the exact-model chassis indicator and pass the
# handler protocol explicitly into the RAM-resident helper.
python3 - "$PKG/files/common.sh" "$PKG/files/fw_update" <<'PYTEST'
from pathlib import Path
import sys
common=Path(sys.argv[1]).read_text()
update=Path(sys.argv[2]).read_text()
assert common.index('STATUS_LED_AVAILABLE') < common.index('PORT_LED_AVAILABLE')
assert update.index('STATUS_LED_AVAILABLE') < update.index('PORT_LED_AVAILABLE', update.index('STATUS_LED_AVAILABLE'))
assert '--status-led-protocol' in update
assert '${STATUS_LED_PROTOCOL:-plain-bool}' in update
PYTEST


python3 - "$PKG/files/fw_factory_reset" "$PKG/fwflash.c" <<'PY_RESET_CONTRACT'
from pathlib import Path
import sys
script=Path(sys.argv[1]).read_text(); helper=Path(sys.argv[2]).read_text()
assert 'flash_erase' not in script
assert 'build_clean_overlay_image' in script
assert '--factory-reset' in script and '--overlay-backup' in script
assert 'quiesce_remaining_userspace' in script
assert 'LED_ANIMATOR_PID' in script
assert '--status-led-green' in script and '--status-led-orange' in script
assert 'factory_reset_operation = true' in helper
assert 'if (factory_reset_operation)' in helper
assert 'Factory-default overlay verified' in helper
assert "is_live_boot && fw_die 'factory reset is disabled while running from PMOSLIVE'" in script
PY_RESET_CONTRACT

python3 - "$PKG/files/fw_update" <<'PY_LIVE_UPDATE_CONTRACT'
from pathlib import Path
import sys
script=Path(sys.argv[1]).read_text()
assert 'if is_live_boot' in script
assert 'VERIFY_ONLY' in script and 'DRY_RUN' in script
assert 'flash writes are disabled while running from PMOSLIVE' in script
PY_LIVE_UPDATE_CONTRACT

# A button-triggered reset owns the reset indicator before entering the factory
# reset script.  If an updater wins the common lock race, cleanup must adopt and
# release that ownership instead of leaving an orphaned animator behind.
mkdir -p "$TMP/reset-bin"
cat >"$TMP/reset-common.sh" <<EOF_RESET_COMMON
#!/bin/sh
fw_die() { printf '%s\n' "\$*" >&2; exit 1; }
need_cmd() { :; }
check_board() { :; }
check_mtd_layout() { OVERLAY_MTD=mtd3; }
acquire_lock() { exit 1; }
release_lock() { printf '%s\n' release-lock >> "$TMP/reset-calls"; }
status_write() { :; }
EOF_RESET_COMMON
cat >"$TMP/reset-bin/postmerkos-ledctl" <<EOF_RESET_LED
#!/bin/sh
case "\${1:-}" in
  status) echo reset ;;
  *) printf '%s\n' "\$*" >> "$TMP/reset-calls" ;;
esac
EOF_RESET_LED
chmod +x "$TMP/reset-bin/postmerkos-ledctl"
sed "s#^\. /usr/lib/fwupdate/common.sh#\. $TMP/reset-common.sh#" \
  "$PKG/files/fw_factory_reset" >"$TMP/fw_factory_reset-test"
chmod +x "$TMP/fw_factory_reset-test"
PATH="$TMP/reset-bin:$PATH" "$TMP/fw_factory_reset-test" --yes >/dev/null 2>&1 || true
grep -q '^error$' "$TMP/reset-calls"
grep -q '^release reset$' "$TMP/reset-calls"
grep -q '^release-lock$' "$TMP/reset-calls"

printf '%s\n' 'fwupdate helper, manifest, MTD geometry, and chassis LED tests passed'
