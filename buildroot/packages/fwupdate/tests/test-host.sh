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

printf '%s\n' 'fwupdate helper, manifest, escaping and MTD geometry tests passed'
