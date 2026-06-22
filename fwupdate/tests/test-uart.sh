#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PKG=$(CDPATH= cd -- "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

cc -std=c99 -Wall -Wextra -Werror -o "$TMP/fwserialrx" "$PKG/fwserialrx.c"
python3 - "$TMP/firmware.input" "$TMP/manifest.input" <<'PY'
from pathlib import Path
import json, sys
firmware, manifest = map(Path, sys.argv[1:])
firmware.write_bytes(bytes(range(256)) * 37 + b'postmerkOS-UART')
manifest.write_text(json.dumps({
    'version': 'uart-self-test',
    'artifact': {'filename': 'firmware.bin'},
    'models': {'MS42P': 'untested'},
}, sort_keys=True) + '\n', encoding='utf-8')
PY
python3 - "$TMP/firmware.input" "$TMP/manifest.input" > "$TMP/frames" <<'PY'
import base64, binascii, hashlib, os, sys

def emit(kind, path):
    data = open(path, 'rb').read()
    print(f"PMOSUART/1 BEGIN {kind} {len(data)} {hashlib.sha256(data).hexdigest()} {os.path.basename(path)}")
    frames = 0
    for offset in range(0, len(data), 1024):
        block = data[offset:offset+1024]
        encoded = base64.b64encode(block).decode('ascii')
        crc = binascii.crc32(block) & 0xffffffff
        line = f"PMOSUART/1 DATA {kind} {frames} {crc:08x} {encoded}"
        print(line)
        if frames == 0:  # Exercise duplicate-last-frame acknowledgement.
            print(line)
        frames += 1
    print(f"PMOSUART/1 END {kind} {frames}")

emit('firmware', sys.argv[1])
emit('manifest', sys.argv[2])
print('PMOSUART/1 DONE')
PY

"$TMP/fwserialrx" \
    --firmware "$TMP/firmware.bin" \
    --manifest "$TMP/firmware.manifest.json" \
    --metadata "$TMP/received.env" \
    --idle-timeout 5 < "$TMP/frames" > "$TMP/protocol.out"

cmp "$TMP/firmware.input" "$TMP/firmware.bin"
cmp "$TMP/manifest.input" "$TMP/firmware.manifest.json"
grep -q '^PMOSUART/1 READY ' "$TMP/protocol.out"
grep -q '^PMOSUART/1 ACK firmware 0$' "$TMP/protocol.out"
[ "$(grep -c '^PMOSUART/1 ACK firmware 0$' "$TMP/protocol.out")" -eq 2 ]
grep -q '^PMOSUART/1 OBJECT-OK firmware ' "$TMP/protocol.out"
grep -q '^PMOSUART/1 OBJECT-OK manifest ' "$TMP/protocol.out"
grep -q '^PMOSUART/1 COMPLETE firmware=' "$TMP/protocol.out"
grep -q '^protocol=1$' "$TMP/received.env"
grep -q '^manifest_received=1$' "$TMP/received.env"

# Corrupt CRC must fail closed and leave no completed object.
sed '0,/PMOSUART\/1 DATA firmware 0 /s/ [0-9a-f][0-9a-f]* / deadbeef /' \
    "$TMP/frames" > "$TMP/corrupt.frames"
if "$TMP/fwserialrx" --firmware "$TMP/bad.bin" --manifest "$TMP/bad.json" \
   --metadata "$TMP/bad.env" --idle-timeout 5 \
   < "$TMP/corrupt.frames" > "$TMP/corrupt.out" 2>&1; then
    echo 'corrupt UART frame unexpectedly succeeded' >&2
    exit 1
fi
[ ! -e "$TMP/bad.bin" ]
[ ! -e "$TMP/bad.env" ]

echo 'PMOSUART/1 framing, duplicate ACK, CRC and whole-object reconstruction tests passed'
