#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
need unsquashfs
need python3

ROOTFS=${1:-$ARTIFACTS_DIR/rootfs.squashfs}
[[ -f "$ROOTFS" ]] || die "Rootfs image not found: $ROOTFS"
TMP=$(mktemp -d)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT
unsquashfs -quiet -d "$TMP/root" "$ROOTFS"
python3 "$VENDOR_MODULE_TOOL" verify "$TMP/root/lib/modules" \
  --required-file "$VENDOR_MODULE_REQUIRED"
count=$(wc -l < "$TMP/root/lib/modules/postmerkos-all-modules.txt" | tr -d ' ')
printf 'PASS: rootfs contains %s hash-verified vendor kernel objects across Luton26, Jaguar1, and Jaguar Dual\n' "$count"
