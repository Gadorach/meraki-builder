#!/usr/bin/env python3
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
build_all = (root / "scripts/build-all.sh").read_text()
build_rootfs = (root / "scripts/build-rootfs.sh").read_text()
validate = (root / "scripts/validate-image.sh").read_text()
init = (root / "buildroot/board/meraki/ms220/overlay/etc/init.d/S15configd").read_text()
marker = root / "buildroot/features/web-overlay/etc/postmerkos/features/web-ui"
configd_dir = root / "buildroot/packages/configd"

assert build_all.count('CLEAN_BUILDROOT="${CLEAN_BUILDROOT:-0}"') >= 2
for text in ('.ms42p-built-ui-mode', '.ms42p-configd-build-fingerprint', 'make configd-dirclean', 'run_logged buildroot-configd'):
    assert text in build_rootfs, text
for text in ('websocket: enabled', 'libwebsockets', 'WebSocket-disabled configd'):
    assert text in validate, text
assert 'websocket_required' in init and 'web image contains WebSocket-disabled configd' in init
assert marker.is_file()

# With pipefail enabled, grep -q can close producer pipelines early and turn a
# valid strings/readelf match into status 141. Validators must consume all input.
for forbidden in (
    "strings \"$VERIFY_DIR/bin/configd\" | grep -Fxq",
    "readelf -d \"$VERIFY_DIR/bin/configd\" | grep -Fq",
    "strings \"$VERIFY_DIR/usr/bin/postmerkosctl\" | grep -Fq",
):
    assert forbidden not in validate, forbidden
assert "grep -Fx 'websocket: enabled' >/dev/null" in validate

# Buildroot passes CPPFLAGS on the make command line. The package Makefile must
# use GNU make's `override` directive or its feature macros are silently lost.
dry_run = subprocess.run(
    ["make", "-Bn", "configd", "CPPFLAGS=-DTEST_FROM_BUILDROOT=1", "ENABLE_WEBSOCKET=1"],
    cwd=configd_dir,
    check=True,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
).stdout
for flag in (
    "-DTEST_FROM_BUILDROOT=1",
    "-D_POSIX_C_SOURCE=200809L",
    "-D_DEFAULT_SOURCE",
    "-DCONFIGD_ENABLE_WEBSOCKET=1",
):
    assert flag in dry_run, f"configd compile command lost {flag}:\n{dry_run}"

print('Buildroot WebSocket/cache contract tests passed')
