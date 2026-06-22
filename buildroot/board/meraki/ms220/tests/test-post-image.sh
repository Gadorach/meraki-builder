#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
POST_IMAGE=$(cd -- "$HERE/.." && pwd)/post-image.sh
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/host/sbin" "$TMP/bin" "$TMP/target/etc"
cat > "$TMP/host/sbin/mkfs.jffs2" <<'MOCK'
#!/bin/sh
set -eu
pad= output=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --pad=*) pad=${1#--pad=} ;;
    -o) output=$2; shift ;;
  esac
  shift
done
[ -n "$pad" ] && [ -n "$output" ]
truncate -s "$pad" "$output"
MOCK
cat > "$TMP/bin/readelf" <<'MOCK'
#!/bin/sh
printf '  Entry point address:               0x81000000\n'
MOCK
cat > "$TMP/bin/mkvcoreiii_payload.py" <<'PACKER'
#!/usr/bin/env python3
import argparse, json, struct, sys, zlib
from pathlib import Path
H=struct.Struct('<8I'); MAGIC=0x4d495053
def parse_int(v): return int(v, 0)
def crc(words, payload):
    words=list(words); words[4]=0
    return zlib.crc32(H.pack(*words)+payload)&0xffffffff
p=argparse.ArgumentParser(); sub=p.add_subparsers(dest='cmd', required=True)
pack=sub.add_parser('pack'); pack.add_argument('--input',type=Path,required=True); pack.add_argument('--output',type=Path,required=True); pack.add_argument('--load-address',type=parse_int,required=True); pack.add_argument('--entry-point',type=parse_int,required=True); pack.add_argument('--alignment',type=parse_int,default=32); pack.add_argument('--max-payload-size',type=parse_int,required=True); pack.add_argument('--metadata',type=Path)
verify=sub.add_parser('verify'); verify.add_argument('image',type=Path); verify.add_argument('--alignment',type=parse_int,default=32); verify.add_argument('--max-payload-size',type=parse_int,required=True)
a=p.parse_args()
if a.cmd=='pack':
    raw=a.input.read_bytes(); payload=raw+b'\0'*((-len(raw))%a.alignment)
    if not payload or len(payload)>a.max_payload_size: raise SystemExit(1)
    words=(MAGIC,a.load_address,len(payload),a.entry_point,0,0,0,0); value=crc(words,payload); words=words[:4]+(value,)+words[5:]
    a.output.write_bytes(H.pack(*words)+payload)
    if a.metadata: a.metadata.write_text(json.dumps({'format':'postmerkos.vcoreiii-payload.v1','payload_size':len(payload),'padding':len(payload)-len(raw),'crc32':f'0x{value:08x}'})+'\n')
else:
    data=a.image.read_bytes(); words=H.unpack_from(data); payload=data[H.size:]
    if len(data)!=H.size+words[2] or words[0]!=MAGIC or words[2]%a.alignment or words[2]>a.max_payload_size or crc(words,payload)!=words[4]: raise SystemExit(1)
PACKER
chmod 0755 "$TMP/host/sbin/mkfs.jffs2" "$TMP/bin/readelf" "$TMP/bin/mkvcoreiii_payload.py"
truncate -s $((0x40000)) "$TMP/loader.bin"
printf 'kernel' > "$TMP/kernel.bin"
printf 'elf' > "$TMP/kernel.elf"
cat > "$TMP/target/etc/postmerkos-release.json" <<'JSON'
{"version":"image-test","target_family":"vcore3","models":{"MS42P":"untested"}}
JSON

make_rootfs() {
  python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
path, size = Path(sys.argv[1]), int(sys.argv[2])
chunk = bytes(range(256))
with path.open('wb') as stream:
    whole, remain = divmod(size, len(chunk))
    for _ in range(whole): stream.write(chunk)
    stream.write(chunk[:remain])
PY
}
run_image() {
  local directory=$1 rootfs=$2
  mkdir -p "$directory"
  cp "$rootfs" "$directory/rootfs.squashfs"
  PATH="$TMP/bin:$PATH" BINARIES_DIR="$directory" HOST_DIR="$TMP/host" \
    TARGET_DIR="$TMP/target" TARGET_READELF="$TMP/bin/readelf" \
    MS42P_KERNEL_ELF="$TMP/kernel.elf" MS42P_KERNEL_BIN="$TMP/kernel.bin" \
    MS42P_LOADER="$TMP/loader.bin" MS42P_PAYLOAD_PACKER="$TMP/bin/mkvcoreiii_payload.py" \
    "$POST_IMAGE" >/dev/null
}

make_rootfs "$TMP/full.squashfs" $((0x800000))
run_image "$TMP/full-output" "$TMP/full.squashfs"
dd if="$TMP/full-output/ms42p-firmware.bin" of="$TMP/full.carved" \
   bs=1M skip=3 count=8 status=none
cmp "$TMP/full.squashfs" "$TMP/full.carved"
[ -s "$TMP/full-output/ms42p-firmware.bin.manifest.json" ]
[ -s "$TMP/full-output/ms42p-firmware.bin.manifest.json.sha256" ]

make_rootfs "$TMP/padded.squashfs" $((0x7fe000))
run_image "$TMP/padded-output" "$TMP/padded.squashfs"
head -c $((0x7fe000)) "$TMP/padded-output/rootfs.region" > "$TMP/padded-prefix"
cmp "$TMP/padded.squashfs" "$TMP/padded-prefix"
magic=$(dd if="$TMP/padded-output/rootfs.region" bs=1 skip=$((0x800000-4096)) \
        count=8 status=none)
[ "$magic" = PMOSMETA ]
python3 - "$TMP/padded-output/rootfs.region" <<'PYMETA'
import json
from pathlib import Path
import sys
region = Path(sys.argv[1]).read_bytes()
slot = region[-4096:]
assert slot[:8] == b"PMOSMETA"
length = int(slot[8:16], 16)
assert 0 < length <= 4080
embedded = json.loads(slot[16:16 + length])
assert embedded["version"] == "image-test"
assert embedded["models"]["MS42P"] == "untested"
assert embedded["metadata_profile"] == "embedded-update-index-v1"
assert "recovery" not in embedded
PYMETA

# A large authoritative manifest must not fail image generation or be copied
# verbatim into the fixed 4 KiB fallback trailer.
python3 - "$TMP/target/etc/postmerkos-release.json" <<'PYLARGE'
import json
from pathlib import Path
import sys
path = Path(sys.argv[1])
data = json.loads(path.read_text())
data["recovery"] = {"fixture": "x" * 16384}
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PYLARGE
run_image "$TMP/oversized-output" "$TMP/padded.squashfs"
python3 - "$TMP/oversized-output/rootfs.region" "$TMP/oversized-output/postmerkos-release.json" <<'PYOVERSIZED'
import json
from pathlib import Path
import sys
region = Path(sys.argv[1]).read_bytes()
authoritative = json.loads(Path(sys.argv[2]).read_text())
assert len(Path(sys.argv[2]).read_bytes()) > 4080
assert len(authoritative["recovery"]["fixture"]) == 16384
slot = region[-4096:]
assert slot[:8] == b"PMOSMETA"
length = int(slot[8:16], 16)
assert length <= 4080
embedded = json.loads(slot[16:16 + length])
assert embedded["metadata_profile"] == "embedded-update-index-v1"
assert "recovery" not in embedded
PYOVERSIZED

python3 "$(cd -- "$HERE/../../../../../scripts" && pwd)/validate-vcoreiii-payload.py" \
  "$TMP/full-output/ms42p-firmware.bin" --offset 0x40000 --region-bytes 0x2c0000 \
  --expected-payload "$TMP/kernel.bin" >/dev/null
python3 - "$TMP/full-output/kernel.payload" <<'PYCRC'
from pathlib import Path
import struct, sys, zlib
data=Path(sys.argv[1]).read_bytes(); h=struct.Struct('<8I'); words=h.unpack_from(data); payload=data[h.size:]
assert len(payload)==words[2] and len(payload)%32==0
zero=list(words); zero[4]=0
assert (zlib.crc32(h.pack(*zero)+payload)&0xffffffff)==words[4]
PYCRC

echo 'post-image preserves SquashFS and emits a 32-byte-aligned, CRC-bound meraki-redboot kernel payload'
