#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
PUBLISH="$ROOT/tools/fwupdate-publish/fwupdate-publish.sh"
FLASHER="$ROOT/tools/firmware-flasher/firmware-flasher.sh"
TMP=$(mktemp -d)
TFTP_PID=""
cleanup() {
    [[ -n $TFTP_PID ]] && kill "$TFTP_PID" 2>/dev/null || true
    [[ -n $TFTP_PID ]] && wait "$TFTP_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required command: $1" >&2; exit 1; }; }
for command in bash cc dash file grep pkg-config python3 sha256sum truncate; do need "$command"; done
[[ -x $PUBLISH ]] || { echo "publisher tool not found: $PUBLISH" >&2; exit 1; }
[[ -x $FLASHER ]] || { echo "firmware flasher tool not found: $FLASHER" >&2; exit 1; }

# All normal host tools are independently parseable. Research utilities are
# deliberately outside this review/test surface.
while IFS= read -r -d '' script; do
    bash -n "$script"
done < <(find "$ROOT/tools" -path '*/research' -prune -o -type f -name '*.sh' -print0)

if find "$ROOT/tools" -path '*/research' -prune -o -type f -name 'common.sh' -print -quit | grep -q .; then
    echo 'a shared common.sh remains in the normal tools surface' >&2
    exit 1
fi

for script in \
    "$ROOT/buildroot/packages/fwupdate/files/common.sh" \
    "$ROOT/buildroot/packages/fwupdate/files/fw_update" \
    "$ROOT/buildroot/packages/fwupdate/files/fw_update_http" \
    "$ROOT/buildroot/packages/fwupdate/files/fw_update_tftp" \
    "$ROOT/buildroot/packages/fwupdate/files/fw_update_sftp" \
    "$ROOT/buildroot/packages/fwupdate/files/fw_update_status" \
    "$ROOT/buildroot/packages/postmerkos-console/files/postmerkos-console"; do
    dash -n "$script"
done

python3 - \
    "$ROOT/tools/firmware-flasher/tftp-server.py" \
    "$ROOT/tools/firmware-flasher/serial-runner.py" \
    "$ROOT/scripts/write-artifact-manifest.py" <<'PY_SYNTAX'
from pathlib import Path
import sys
for source in sys.argv[1:]:
    compile(Path(source).read_text(encoding='utf-8'), source, 'exec')
PY_SYNTAX

# Exercise the package's manifest helper tests against the host json-c.
"$ROOT/buildroot/packages/fwupdate/tests/test-host.sh"

cc -static -Os -Wall -Wextra -Werror -std=c99 \
    -o "$TMP/fwflash" "$ROOT/buildroot/packages/fwupdate/fwflash.c"
file "$TMP/fwflash" | grep -qi 'statically linked'

# Build a structurally valid full-image fixture and current artifact bundle.
truncate -s 16777216 "$TMP/test.bin"
printf 'PMOSRAM READY 2' | dd of="$TMP/test.bin" bs=1 seek=$((0x100)) conv=notrunc status=none
printf SPIM | dd of="$TMP/test.bin" bs=1 seek=$((0x40000)) conv=notrunc status=none
printf hsqs | dd of="$TMP/test.bin" bs=1 seek=$((0x300000)) conv=notrunc status=none
mkdir -p "$TMP/recovery"
python3 - "$TMP/test.bin" "$TMP/test.bin.manifest.json" "$TMP/loader.manifest.json" "$TMP/recovery" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import zlib

image, output, loader_output, recovery_raw = sys.argv[1:]
recovery = Path(recovery_raw)
targets = {
    'luton26': {
        'id': 1, 'spi': 0x70000064,
        'models': ['MS22', 'MS22P', 'MS220-8', 'MS220-8P', 'MS220-24', 'MS220-24P'],
    },
    'jaguar1': {
        'id': 2, 'spi': 0x70000068,
        'models': [
            'MS320-24', 'MS320-24P', 'MS220-48', 'MS220-48P', 'MS220-48LP',
            'MS220-48FP', 'MS320-48', 'MS320-48P', 'MS320-48LP', 'MS320-48FP',
            'MS42', 'MS42P',
        ],
    },
}
all_models = [model for target in targets.values() for model in target['models']]

# Turn the sparse file into a complete structurally valid v0.7 fixture.
data = bytearray(Path(image).read_bytes())
cursor = 0x100
for marker in (
    b'PMOSRAM READY 2', b'PMOSBOOT MENU-PROBE',
    b'PMOSBOOT MENU 1=UART-RAMLOADER 2=FW-RECOVERY',
):
    data[cursor:cursor + len(marker)] = marker
    cursor += len(marker) + 16
kernel = b'smoke-kernel'
kernel += bytes((-len(kernel)) % 32)
header = struct.Struct('<8I')
words = [0x4D495053, 0x81000000, len(kernel), 0x81000000, 0, 0, 0, 0]
words[4] = zlib.crc32(header.pack(*words) + kernel) & 0xffffffff
data[0x40000:0x40000 + header.size + len(kernel)] = header.pack(*words) + kernel
data[0x300000:0x300004] = b'hsqs'
Path(image).write_bytes(data)
image_data = bytes(data)
digest = hashlib.sha256(image_data).hexdigest()
manifest = {
    'version': 'test-version',
    'artifact': {
        'filename': os.path.basename(image),
        'bytes': os.path.getsize(image),
        'sha256': digest,
        'updates': ['squashfs', 'jffs2-optional'],
        'bootloader_update': False,
        'kernel_update': False,
    },
    'models': {model: ('validated' if model == 'MS42P' else 'untested') for model in all_models},
}
Path(output).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')

geometry = {'bytes': 0x1000000, 'erase_bytes': 0x10000, 'page_bytes': 256, 'address_bytes': 3}
jedec = ['c22018', 'ef4018', '012018', '20ba18', 'c84018']
embedded = {}
for family, target in targets.items():
    marker = (
        f"PMOSRECOVERY3;SOC={family};FAMILY={target['id']};"
        f"SPI={target['spi']:08x};PROTO=3;PREFLIGHT=4;BAUDTEST=1;FRAME_MAX=4096;"
        "WINDOW_MAX=16;ACKFMT=BIN1;SPARSE=1;LZ4=1;CONFIRM_RETRY=1;"
        "AUTO_CONFIRM=1;AUTO_REBOOT=1;END"
    ).encode()
    payload = recovery / f'recovery-{family}.bin'
    payload.write_bytes(b'smoke-payload\0' + marker + b'\0')
    raw = payload.read_bytes()
    payload_digest = hashlib.sha256(raw).hexdigest()
    embedded[family] = {
        'path': str(payload), 'size': len(raw), 'sha256': payload_digest,
        'load_address': 0x81000000, 'entry_address': 0x81000000,
        'entry_contract': 'flat-binary-byte-zero-v1',
        'manifest_lookup_contract': 'direct-object-members-v1',
        'hardware_preflight_contract': 'spi-nor-scratch-rw-restore-loader-crc-v4',
        'spi_master_enable_contract': 'preserve-general-ctrl-enable-spi-v1',
        'adaptive_transport_contract': 'pmosrec-v3-adaptive-uart-sparse-lz4-v1',
    }
    descriptor = {
        'format': 'postmerkos.uart-recovery-payload.v3',
        'protocol_version': 3,
        'soc_family': family,
        'soc_family_id': target['id'],
        'spi_software_mode_address': target['spi'],
        'accepted_models': target['models'],
        'accepted_flash_bytes': 0x1000000,
        'accepted_jedec_ids': jedec,
        'flash_geometry': geometry,
        'operations': ['verify', 'preflight', 'dry-run', 'flash'],
        'load_address': 0x81000000,
        'entry_address': 0x81000000,
        'entry_contract': 'flat-binary-byte-zero-v1',
        'manifest_lookup_contract': 'direct-object-members-v1',
        'hardware_preflight_contract': 'spi-nor-scratch-rw-restore-loader-crc-v4',
        'spi_master_enable_contract': 'preserve-general-ctrl-enable-spi-v1',
        'preflight_scratch': {'default_address': 0x00FF0000, 'bytes': 0x10000, 'minimum_address': 0x40000, 'restore_original': True},
        'transport_integrity': ['frame-crc32', 'compact-ack-crc32', 'object-crc32', 'object-sha256', 'reconstructed-image-sha256'],
        'adaptive_transport_contract': 'pmosrec-v3-adaptive-uart-sparse-lz4-v1',
        'binary': {
            'filename': payload.name,
            'bytes': len(raw),
            'sha256': payload_digest,
        },
    }
    (recovery / f'recovery-{family}.descriptor.json').write_text(
        json.dumps(descriptor, indent=2, sort_keys=True) + '\n'
    )

loader = image_data[:0x40000]
Path(loader_output).write_text(json.dumps({
    'format': 'postmerkos.vcoreiii-linuxloader-build.v7',
    'variant': 'development',
    'boot_region': {'bytes': len(loader), 'sha256': hashlib.sha256(loader).hexdigest()},
    'policies': {
        'crc': 'warn', 'size': 'legacy-warn',
        'payload_slot_end': 0x300000, 'hard_payload_limit': 0x2bffe0,
    },
    'toolchain': {'id': 'smoke-gcc473'},
    'uart_ramloader': {
        'enabled': True,
        'protocol_version': 2,
        'probe_timeout_ms': 3000,
        'interbyte_timeout_ms': 3000,
        'menu_selection_timeout_ms': 5000,
        'maximum_payload_bytes': 4 * 1024 * 1024,
        'ram_start': 0x81000000,
        'ram_end': 0x87f00000,
        'supported_soc_families': ['luton26', 'jaguar1'],
        'transport_integrity': ['frame-crc32', 'compact-ack-crc32', 'object-crc32', 'object-sha256', 'reconstructed-image-sha256'],
        'adaptive_transport_contract': 'pmosrec-v3-adaptive-uart-sparse-lz4-v1',
        'boot_menu': {
            'probe_timeout_ms': 3000,
            'selection_timeout_ms': 5000,
            'options': {'1': 'uart-ramloader', '2': 'embedded-firmware-recovery'},
            'noise_behavior': 'invalid/no explicit option continues normal boot',
        },
        'image_check_diagnostics': 'structured-pass-warn-fail-skip-values-v1',
        'embedded_recovery': embedded,
    },
}, indent=2, sort_keys=True) + '\n')
PY
(cd "$TMP" && sha256sum test.bin > test.bin.sha256)
(cd "$TMP" && sha256sum test.bin.manifest.json > test.bin.manifest.json.sha256)

# The final publication helper must rewrite Buildroot's generic artifact identity
# to the timestamped image emitted by make all.
printf 'hsqs' > "$TMP/rootfs.squashfs"
cp "$TMP/test.bin" "$TMP/ms42p-postmerkos-test.bin"
python3 "$ROOT/scripts/write-artifact-manifest.py" \
    "$TMP/test.bin.manifest.json" "$TMP/ms42p-postmerkos-test.bin.manifest.json" \
    "$TMP/ms42p-postmerkos-test.bin" "$TMP/rootfs.squashfs" \
    "$TMP/loader.manifest.json" "$TMP/recovery"
python3 - "$TMP/ms42p-postmerkos-test.bin" "$TMP/ms42p-postmerkos-test.bin.manifest.json" <<'PY_PUBLISHED_MANIFEST'
import hashlib, json, os, sys
image, manifest_path = sys.argv[1:]
with open(manifest_path, encoding='utf-8') as stream:
    manifest = json.load(stream)
artifact = manifest['artifact']
assert artifact['filename'] == os.path.basename(image)
assert artifact['bytes'] == os.path.getsize(image)
assert artifact['sha256'] == hashlib.sha256(open(image, 'rb').read()).hexdigest()
assert artifact['supported_flash_scopes'] == ['system', 'full']
assert artifact['default_flash_scope'] == 'system'
assert artifact['regions']['bootloader']['offset'] == 0
assert artifact['regions']['bootloader']['bytes'] == 0x40000
assert len(artifact['regions']['bootloader']['sha256']) == 64
assert manifest['recovery']['uart_firmware']['protocol_version'] == 3
PY_PUBLISHED_MANIFEST

"$PUBLISH" "$TMP/test.bin" "$TMP/published-modern" >/dev/null
(
    cd "$TMP/published-modern"
    sha256sum -c test.bin.sha256 >/dev/null
    sha256sum -c test.bin.manifest.json.sha256 >/dev/null
)
grep -q '^test-version|ms42p|test.bin|16777216|postmerkOS VCore-III firmware$' "$TMP/published-modern/index.tsv"

"$PUBLISH" --checksum-only "$TMP/test.bin" "$TMP/published-checksum" checksum-version 'Checksum-only smoke test' >/dev/null
[[ -f "$TMP/published-checksum/test.bin.sha256" ]]
[[ ! -e "$TMP/published-checksum/test.bin.manifest.json" ]]
grep -q '^checksum-version|ms42p|test.bin|16777216|Checksum-only smoke test$' "$TMP/published-checksum/index.tsv"

"$PUBLISH" --legacy "$TMP/test.bin" "$TMP/published-legacy" legacy-version 'Legacy smoke test' >/dev/null
[[ ! -e "$TMP/published-legacy/test.bin.manifest.json" ]]
grep -q '^legacy-version|ms42p|test.bin|16777216|Legacy smoke test$' "$TMP/published-legacy/index.tsv"

# Verify each host flasher contract produces the intended target command.
bash -s -- "$FLASHER" <<'SH_FLASHER_COMMANDS'
set -euo pipefail
source "$1"
HOST_TFTP_IP=192.0.2.1
TFTP_PORT=1069
STAGED_NAME=test.bin
OVERLAY_POLICY=preserve
OPERATION_ARGS=(--verify-only)
FLASH_SCOPE=system
FIRMWARE_VERSION=2026.06.18
MODE=modern
command=$(remote_command)
[[ $command == *postmerkos-console* && $command == *--manifest-file* && $command != *--no-manifest* ]]
MODE=checksum
command=$(remote_command)
[[ $command == *postmerkos-console* && $command == *--no-manifest* && $command == *--version* && $command != *--manifest-file* ]]
MODE=legacy
FIRMWARE_VERSION=
command=$(remote_command)
[[ $command == fw_update_tftp* && $command == *--yes* && $command != *--full-flash* ]]
MODE=modern
FLASH_SCOPE=full
OVERLAY_POLICY=image
command=$(remote_command)
[[ $command == *--full-flash* && $command == *--accept-full-flash* ]]
SH_FLASHER_COMMANDS

# Exercise the flasher-private TFTP helper without repeating the full flasher
# bundle self-test (which is available separately through --self-test).
mkdir -p "$TMP/tftp-root"
printf 'host TFTP smoke test\n' > "$TMP/tftp-root/test.bin"
TFTP_PORT=$((21000 + RANDOM % 20000))
python3 "$ROOT/tools/firmware-flasher/tftp-server.py" \
    --bind 127.0.0.1 --port "$TFTP_PORT" --root "$TMP/tftp-root" \
    >"$TMP/tftp.log" 2>&1 &
TFTP_PID=$!
sleep 1
kill -0 "$TFTP_PID"
python3 - "$TFTP_PORT" <<'PY_TFTP'
import socket, struct, sys
port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(3)
sock.sendto(struct.pack('!H', 1) + b'test.bin\0octet\0', ('127.0.0.1', port))
data, peer = sock.recvfrom(65535)
opcode, block = struct.unpack('!HH', data[:4])
assert opcode == 3 and block == 1 and b'host TFTP smoke test' in data[4:]
sock.sendto(struct.pack('!HH', 4, block), peer)
PY_TFTP
kill "$TFTP_PID"
wait "$TFTP_PID" 2>/dev/null || true
TFTP_PID=""

# Full-flash argument groups must be complete and cannot silently fall back to
# a rootfs-only update.
set +e
"$TMP/fwflash" --no-reboot \
    --rootfs-image "$TMP/test.bin" --rootfs-mtd /dev/null --rootfs-backup "$TMP/test.bin" \
    --kernel-image "$TMP/test.bin" --kernel-mtd /dev/null --kernel-backup "$TMP/test.bin" \
    --status-file "$TMP/full-status.json" --log-file "$TMP/full-flash.log" >/dev/null 2>&1
full_rc=$?
set -e
[[ $full_rc -eq 2 ]]

# Exercise fwflash's non-destructive preflight failure.
set +e
"$TMP/fwflash" --no-reboot \
    --rootfs-image "$TMP/test.bin" \
    --rootfs-mtd /dev/null \
    --rootfs-backup "$TMP/test.bin" \
    --status-file "$TMP/status.json" \
    --log-file "$TMP/flash.log" \
    --source host-test --firmware test.bin >/dev/null 2>&1
rc=$?
set -e
[[ $rc -ne 0 ]]
grep -q '"state":"error"' "$TMP/status.json"
grep -q '"source":"host-test"' "$TMP/status.json"

printf 'All host tool and updater smoke tests passed. Hardware access was not exercised.\n'
