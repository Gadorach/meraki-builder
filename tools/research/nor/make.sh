#!/usr/bin/env bash
# Compatibility helper for low-level NOR research. Normal images are produced
# by buildroot/board/meraki/ms220/post-image.sh.
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LOADER="${LOADER:-$REPO_ROOT/artifacts/loader1.bin}"
PAYLOAD_PACKER="${PAYLOAD_PACKER:-$REPO_ROOT/artifacts/tools/mkvcoreiii_payload.py}"
KERNEL_BIN="${KERNEL_BIN:-$SCRIPT_DIR/vmlinuz.bin}"
ROOTFS="${ROOTFS:-$SCRIPT_DIR/bin/bootubi.new}"
JFFS2="${JFFS2:-$SCRIPT_DIR/bin/jffs2}"
OUTPUT="${OUTPUT:-$SCRIPT_DIR/switch-new.bin}"
WORK="${WORK:-$SCRIPT_DIR/bin/.image-work}"

LOADER_REGION=$((0x040000))
KERNEL_REGION=$((0x2c0000))
ROOTFS_REGION=$((0x800000))
JFFS2_REGION=$((0x500000))
TOTAL_SIZE=$((0x1000000))
MAX_KERNEL_PAYLOAD=$((KERNEL_REGION - 32))

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
for file in "$LOADER" "$PAYLOAD_PACKER" "$KERNEL_BIN" "$ROOTFS" "$JFFS2"; do
  [[ -f "$file" ]] || fail "missing required input: $file"
done
[[ $(stat -c %s "$LOADER") -eq $LOADER_REGION ]] || fail "loader must be exactly 256 KiB"
[[ $(stat -c %s "$ROOTFS") -le $ROOTFS_REGION ]] || fail "SquashFS exceeds 8 MiB"
[[ $(stat -c %s "$JFFS2") -eq $JFFS2_REGION ]] || fail "JFFS2 region must be exactly 5 MiB"

rm -rf "$WORK"
mkdir -p "$WORK"
python3 "$PAYLOAD_PACKER" pack \
  --input "$KERNEL_BIN" --output "$WORK/kernel.payload" \
  --load-address 0x81000000 --entry-point 0x81000000 \
  --alignment 32 --max-payload-size "$MAX_KERNEL_PAYLOAD" \
  --metadata "$WORK/kernel-payload.json"
python3 "$PAYLOAD_PACKER" verify "$WORK/kernel.payload" \
  --alignment 32 --max-payload-size "$MAX_KERNEL_PAYLOAD"

cp "$WORK/kernel.payload" "$WORK/kernel.region"
truncate -s "$KERNEL_REGION" "$WORK/kernel.region"
cp "$ROOTFS" "$WORK/rootfs.region"
truncate -s "$ROOTFS_REGION" "$WORK/rootfs.region"
cat "$LOADER" "$WORK/kernel.region" "$WORK/rootfs.region" "$JFFS2" > "$OUTPUT"
[[ $(stat -c %s "$OUTPUT") -eq $TOTAL_SIZE ]] || fail "generated image is not exactly 16 MiB"
python3 "$REPO_ROOT/scripts/validate-vcoreiii-payload.py" "$OUTPUT" \
  --expected-payload "$KERNEL_BIN" --json-output "$WORK/kernel-validation.json"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
printf 'Generated %s from source-built meraki-redboot with validated SPIM CRC/alignment.\n' "$OUTPUT"
