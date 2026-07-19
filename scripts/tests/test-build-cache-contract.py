#!/usr/bin/env python3
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
build_all = (root / "scripts/build-all.sh").read_text()
build_rootfs = (root / "scripts/build-rootfs.sh").read_text()
validate = (root / "scripts/validate-image.sh").read_text()
prepare = (root / "scripts/prepare-buildroot.sh").read_text()
buildroot_config = (root / "buildroot/board/meraki/ms220/buildroot-config").read_text()
init = (root / "buildroot/board/meraki/ms220/overlay/etc/init.d/S15configd").read_text()
marker = root / "buildroot/features/web-overlay/etc/postmerkos/features/web-ui"
configd_dir = root / "buildroot/packages/configd"

assert build_all.count('CLEAN_BUILDROOT="${CLEAN_BUILDROOT:-0}"') >= 2
for token in (
    'kernel-build-contract.py" verify',
    'Existing kernel artifacts do not satisfy the current PMOSLIVE boot-argument contract',
    'Reusing verified PMOSLIVE-compatible kernel artifacts',
):
    assert token in build_all, token

for variable in ("REBUILD_KERNEL", "REBUILD_LOADER", "CLEAN_KERNEL", "LOADER_REPO_URL", "LOADER_REF"):
    token = f'{variable}="${{{variable}:-'
    assert build_all.count(token) >= 2, f"Distrobox handoff does not preserve {variable}"
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


for token in (
    "BR2_PACKAGE_AVAHI=y",
    "BR2_PACKAGE_AVAHI_DAEMON=y",
    "# BR2_PACKAGE_AVAHI_AUTOIPD is not set",
    "# BR2_PACKAGE_AVAHI_DEFAULT_SERVICES is not set",
    "# BR2_PACKAGE_DBUS is not set",
):
    assert token in buildroot_config, token
for token in (
    "Buildroot did not retain Avahi",
    "Buildroot did not retain the Avahi daemon",
    "Buildroot did not retain Avahi's libdaemon dependency",
    "Buildroot did not retain Avahi's Expat dependency",
    "Avahi auto-IP must remain disabled",
    "Avahi default service advertisements must remain disabled",
    "D-Bus must remain disabled",
):
    assert token in prepare, token

print('Buildroot WebSocket/cache and compact mDNS contract tests passed')
