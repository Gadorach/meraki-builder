#!/usr/bin/env bash
set -Eeuo pipefail

: "${BINARIES_DIR:?Buildroot BINARIES_DIR is not set}"
: "${HOST_DIR:?Buildroot HOST_DIR is not set}"
: "${MS42P_KERNEL_ELF:?MS42P_KERNEL_ELF is not set}"
: "${MS42P_KERNEL_BIN:?MS42P_KERNEL_BIN is not set}"
: "${MS42P_LOADER:?MS42P_LOADER is not set}"
: "${MS42P_PAYLOAD_PACKER:?MS42P_PAYLOAD_PACKER is not set}"

ROOTFS="$BINARIES_DIR/rootfs.squashfs"
OUTPUT="$BINARIES_DIR/ms42p-firmware.bin"
WORK="$BINARIES_DIR/ms42p-image-parts"
MKFS_JFFS2="$HOST_DIR/sbin/mkfs.jffs2"

LOADER_REGION=$((0x040000))
KERNEL_REGION=$((0x2c0000))
ROOTFS_REGION=$((0x800000))
JFFS2_REGION=$((0x500000))
TOTAL_SIZE=$((0x1000000))
LOAD_ADDRESS=$((0x81000000))
MAX_KERNEL_PAYLOAD=$((KERNEL_REGION - 32))

for input in "$MS42P_KERNEL_ELF" "$MS42P_KERNEL_BIN" "$MS42P_LOADER" "$MS42P_PAYLOAD_PACKER" "$ROOTFS"; do
    [[ -f "$input" ]] || { echo "Missing image input: $input" >&2; exit 1; }
done
[[ -x "$MKFS_JFFS2" ]] || { echo "Missing Buildroot host mkfs.jffs2: $MKFS_JFFS2" >&2; exit 1; }

loader_size="$(stat -c %s "$MS42P_LOADER")"
kernel_size="$(stat -c %s "$MS42P_KERNEL_BIN")"
rootfs_size="$(stat -c %s "$ROOTFS")"
READELF="${TARGET_READELF:-${CROSS_COMPILE:-}readelf}"
command -v "$READELF" >/dev/null 2>&1 || READELF=readelf
entry="$("$READELF" -h "$MS42P_KERNEL_ELF" | awk '/Entry point address/ {print $4}')"
rootfs_remaining=$((ROOTFS_REGION - rootfs_size))
rootfs_percent="$(awk -v used="$rootfs_size" -v max="$ROOTFS_REGION" 'BEGIN { printf "%.2f", used * 100 / max }')"
{
    printf 'SquashFS bytes used: %s\n' "$rootfs_size"
    printf 'SquashFS maximum:    %s\n' "$ROOTFS_REGION"
    printf 'SquashFS usage:      %s%%\n' "$rootfs_percent"
    printf 'SquashFS remaining:  %s\n' "$rootfs_remaining"
    if [[ -n "${TARGET_DIR:-}" && -d "${TARGET_DIR:-}" ]]; then
        printf '\nLargest target files:\n'
        find "$TARGET_DIR" -type f -printf '%s %p\n' | sort -nr | sed -n '1,40p'
    fi
} | tee "$BINARIES_DIR/squashfs-usage.txt"

[[ "$loader_size" -eq "$LOADER_REGION" ]] || { echo "Loader must be exactly 256 KiB" >&2; exit 1; }
(( kernel_size > 0 && kernel_size <= MAX_KERNEL_PAYLOAD )) || {
    echo "Kernel exceeds the 2816 KiB region before loader-required padding/header" >&2
    exit 1
}
(( rootfs_size > 0 && rootfs_size <= ROOTFS_REGION )) || { echo "SquashFS exceeds the 8 MiB region" >&2; exit 1; }
[[ "$entry" == 0x81000000 ]] || { echo "Unexpected kernel entry point: $entry" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/jffs2-root/.upper/etc" "$WORK/jffs2-root/.work/etc" \
    "$WORK/jffs2-root/.upper/root" "$WORK/jffs2-root/.work/root"

# The selected meraki-redboot source release owns the payload format. Its packer
# pads the compressed kernel to 32 bytes and calculates IEEE CRC-32 over the
# header with the CRC word zeroed plus the complete padded payload.
python3 "$MS42P_PAYLOAD_PACKER" pack \
    --input "$MS42P_KERNEL_BIN" \
    --output "$WORK/kernel.payload" \
    --load-address "$LOAD_ADDRESS" \
    --entry-point "$LOAD_ADDRESS" \
    --alignment 32 \
    --max-payload-size "$MAX_KERNEL_PAYLOAD" \
    --metadata "$WORK/kernel-payload.json"
python3 "$MS42P_PAYLOAD_PACKER" verify "$WORK/kernel.payload" \
    --alignment 32 --max-payload-size "$MAX_KERNEL_PAYLOAD"

payload_size="$(stat -c %s "$WORK/kernel.payload")"
(( payload_size <= KERNEL_REGION )) || { echo "Packed kernel exceeds the 2816 KiB region" >&2; exit 1; }
head -c 32 "$WORK/kernel.payload" > "$WORK/boot1-header.bin"
cp "$WORK/kernel.payload" "$WORK/kernel.region"
truncate -s "$KERNEL_REGION" "$WORK/kernel.region"

cp "$ROOTFS" "$WORK/rootfs.region"
truncate -s "$ROOTFS_REGION" "$WORK/rootfs.region"
# Embed metadata only in padding that was never part of the SquashFS image.
# An exactly 8 MiB SquashFS must remain byte-for-byte intact; in that case the
# sidecar manifest generated below is authoritative.
if [[ -n "${TARGET_DIR:-}" && -f "$TARGET_DIR/etc/postmerkos-release.json" && "$rootfs_remaining" -ge 4096 ]]; then
    python3 - "$WORK/rootfs.region" "$TARGET_DIR/etc/postmerkos-release.json" "$ROOTFS_REGION" <<'PYMETA'
from pathlib import Path
import json
import sys

image = Path(sys.argv[1])
source_path = Path(sys.argv[2])
region = int(sys.argv[3])
slot = 4096

# The complete release manifest grows as recovery and hardware capabilities are
# added. The fixed 4 KiB trailer is only a fallback update index; the complete
# manifest remains inside the SquashFS and in the checksummed sidecar artifact.
source = json.loads(source_path.read_text(encoding="utf-8"))
required = ("version", "target_family", "image_format", "config_schema",
            "fwupdate_api", "web_api", "models")
missing = [key for key in ("version", "models") if key not in source]
if missing:
    raise SystemExit("postmerkOS release manifest lacks embedded-index field(s): " + ", ".join(missing))
embedded = {key: source[key] for key in required if key in source}
embedded["metadata_profile"] = "embedded-update-index-v1"
manifest = json.dumps(embedded, sort_keys=True, separators=(",", ":")).encode("utf-8")
if len(manifest) > slot - 16:
    print(
        f"postmerkOS embedded update index is {len(manifest)} bytes and exceeds "
        f"the {slot - 16}-byte trailer payload; trailer omitted, sidecar remains authoritative.",
        file=sys.stderr,
    )
    raise SystemExit(0)
payload = b"PMOSMETA" + f"{len(manifest):08x}".encode() + manifest
with image.open("r+b") as stream:
    stream.seek(region - slot)
    stream.write(payload)
print(f"Embedded compact postmerkOS update index: {len(manifest)} bytes")
PYMETA
elif [[ -n "${TARGET_DIR:-}" && -f "$TARGET_DIR/etc/postmerkos-release.json" ]]; then
    printf '%s\n' 'SquashFS leaves less than 4 KiB padding; embedded metadata omitted.'
fi

"$MKFS_JFFS2" --pad="$JFFS2_REGION" -l -n -X lzo -x zlib -y 40:lzo \
    -r "$WORK/jffs2-root" -o "$WORK/overlay.region"

cat "$MS42P_LOADER" "$WORK/kernel.region" "$WORK/rootfs.region" \
    "$WORK/overlay.region" > "$OUTPUT"
[[ "$(stat -c %s "$OUTPUT")" -eq "$TOTAL_SIZE" ]] || {
    echo "Generated firmware is not exactly 16 MiB" >&2
    exit 1
}

cp -f "$WORK/boot1-header.bin" "$WORK/kernel.payload" "$WORK/kernel-payload.json" \
    "$WORK/kernel.region" "$WORK/rootfs.region" "$WORK/overlay.region" "$BINARIES_DIR/"
if [[ -n "${TARGET_DIR:-}" && -f "$TARGET_DIR/etc/postmerkos-release.json" ]]; then
    cp -f "$TARGET_DIR/etc/postmerkos-release.json" "$BINARIES_DIR/postmerkos-release.json"
fi
(cd "$BINARIES_DIR" && sha256sum "$(basename "$OUTPUT")" > "$(basename "$OUTPUT").sha256")
if [[ -n "${TARGET_DIR:-}" && -f "$TARGET_DIR/etc/postmerkos-release.json" ]]; then
    python3 - "$TARGET_DIR/etc/postmerkos-release.json" "$OUTPUT" "$OUTPUT.manifest.json" <<'PYMANIFEST'
import hashlib
import json
import os
import sys
release_path, image_path, output_path = sys.argv[1:]
with open(release_path, encoding='utf-8') as stream:
    manifest = json.load(stream)
hash_value = hashlib.sha256()
with open(image_path, 'rb') as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        hash_value.update(block)
manifest['artifact'] = {
    'filename': os.path.basename(image_path),
    'bytes': os.path.getsize(image_path),
    'sha256': hash_value.hexdigest(),
}
with open(output_path, 'w', encoding='utf-8', newline='\n') as stream:
    json.dump(manifest, stream, indent=2, sort_keys=True)
    stream.write('\n')
PYMANIFEST
    (cd "$BINARIES_DIR" && sha256sum "$(basename "$OUTPUT").manifest.json" > "$(basename "$OUTPUT").manifest.json.sha256")
fi
printf 'Generated %s\n' "$OUTPUT"
