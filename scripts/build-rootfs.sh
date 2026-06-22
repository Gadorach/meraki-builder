#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"
load_build_state
for cmd in strings readelf; do need "$cmd"; done

# Keep Buildroot host tools in the same supported Ubuntu environment as the
# pinned kernel toolchain. This also protects direct `make rootfs` invocations
# that do not pass through build-all.sh.
if [[ "${MS42P_IN_DISTROBOX:-0}" != 1 ]] && ! bool_enabled "${ALLOW_UNSUPPORTED_HOST_BUILD:-0}"; then
  if bool_enabled "${USE_DISTROBOX:-0}"; then
    exec "$SCRIPT_DIR/distrobox-run.sh" env \
      INCLUDE_UI="${INCLUDE_UI:-0}" \
      CLEAN_BUILDROOT="${CLEAN_BUILDROOT:-0}" \
      ./scripts/build-rootfs.sh
  elif command -v pacman >/dev/null 2>&1 && command -v distrobox >/dev/null 2>&1; then
    if ask_yes_no "Build the root filesystem in Ubuntu 22.04 Distrobox?" yes; then
      exec "$SCRIPT_DIR/distrobox-run.sh" env \
        INCLUDE_UI="${INCLUDE_UI:-0}" \
        CLEAN_BUILDROOT="${CLEAN_BUILDROOT:-0}" \
        ./scripts/build-rootfs.sh
    fi
    warn "Continuing with the unsupported Arch/CachyOS host compiler."
  fi
fi

[[ -f "$BUILDROOT_DIR/.config" ]] || die "Buildroot is not prepared. Run make prepare first."
[[ -f "$KERNEL_ARTIFACT_DIR/vmlinuz" ]] || die "Missing kernel ELF"
[[ -f "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" ]] || die "Missing compressed kernel binary"
[[ -f "$LOADER_ARTIFACT" ]] || die "Missing source-built LinuxLoader boot region"
[[ -f "$LOADER_MANIFEST" ]] || die "Missing LinuxLoader capability manifest"

# Refresh and validate both module staging paths for direct build-rootfs.sh
# invocations and reused Buildroot output trees.
"$SCRIPT_DIR/stage-vendor-modules.sh" \
  "$GENERATED_OVERLAY/lib/modules" \
  "$BUILDROOT_DIR/board/meraki/ms220/vendor-modules"

cd "$BUILDROOT_DIR"

# Buildroot compiles a substantial set of host utilities.  Reusing output made
# by another distribution/compiler is unsafe.  Track the environment and clean
# automatically when it changes.  An existing untracked output tree is cleaned
# once, which also recovers from the GCC 16 host-binutils failure.
env_stamp="$BUILDROOT_DIR/.ms42p-build-environment"
os_id="unknown"
os_version="unknown"
if [[ -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
  os_id="${ID:-unknown}"
  os_version="${VERSION_ID:-unknown}"
fi
host_cc="$(gcc -dumpfullversion -dumpversion 2>/dev/null || printf unknown)"
if [[ "${MS42P_IN_DISTROBOX:-0}" == 1 ]]; then
  current_env="distrobox:${DISTROBOX_IMAGE}:${os_id}:${os_version}:gcc-${host_cc}"
else
  current_env="host:${os_id}:${os_version}:gcc-${host_cc}"
fi

need_environment_clean=0
if [[ -d output/build ]]; then
  if [[ ! -f "$env_stamp" ]]; then
    warn "Existing Buildroot output has no build-environment stamp; cleaning it before reuse."
    need_environment_clean=1
  elif [[ "$(cat "$env_stamp")" != "$current_env" ]]; then
    warn "Buildroot environment changed from '$(cat "$env_stamp")' to '$current_env'; cleaning output."
    need_environment_clean=1
  fi
fi

current_ui_mode=0
bool_enabled "${INCLUDE_UI:-0}" && current_ui_mode=1
ui_mode_stamp="$BUILDROOT_DIR/.ms42p-built-ui-mode"
need_ui_mode_clean=0
if [[ -d output/build ]]; then
  if [[ ! -f "$ui_mode_stamp" ]]; then
    warn "Existing Buildroot output predates UI-mode tracking; cleaning it once before reuse."
    need_ui_mode_clean=1
  elif [[ "$(cat "$ui_mode_stamp")" != "$current_ui_mode" ]]; then
    warn "Buildroot UI mode changed from '$(cat "$ui_mode_stamp")' to '$current_ui_mode'; cleaning output to remove stale package files."
    need_ui_mode_clean=1
  fi
fi

if bool_enabled "${CLEAN_BUILDROOT:-0}" || (( need_environment_clean || need_ui_mode_clean )); then
  log "Cleaning Buildroot output while preserving the download cache"
  make clean
fi
printf '%s\n' "$current_env" > "$env_stamp"

# Buildroot local packages use fixed versions and local source directories, so
# source edits do not invalidate completed package stamps automatically.
configd_fingerprint_stamp="$BUILDROOT_DIR/.ms42p-configd-build-fingerprint"
current_configd_fingerprint="$(python3 - "$BUILDROOT_DIR/package/configd" "$BUILDROOT_DIR/.config" <<'PY_CONFIGD_FINGERPRINT'
import hashlib
import sys
from pathlib import Path
package = Path(sys.argv[1])
config = Path(sys.argv[2])
h = hashlib.sha256()
for path in sorted(p for p in package.rglob('*') if p.is_file()):
    h.update(path.relative_to(package).as_posix().encode() + b'\0')
    h.update(path.read_bytes())
for line in config.read_text(errors='replace').splitlines():
    if line.startswith('BR2_PACKAGE_CONFIGD') or line.startswith('# BR2_PACKAGE_CONFIGD'):
        h.update(line.encode() + b'\n')
print(h.hexdigest())
PY_CONFIGD_FINGERPRINT
)"
previous_configd_fingerprint=''
[[ -f "$configd_fingerprint_stamp" ]] && previous_configd_fingerprint="$(cat "$configd_fingerprint_stamp")"
if [[ "$previous_configd_fingerprint" != "$current_configd_fingerprint" ]]; then
  warn "configd source or feature selection changed; a fresh local-package build is required."
fi

log "Prefetching Buildroot sources"
run_logged buildroot-download \
  make -j1 BR2_DL_DIR="$BUILDROOT_DL_DIR" \
  BR2_PRIMARY_SITE="https://sources.buildroot.net" source

# configd is maintained as a fixed-version local Buildroot package. Buildroot's
# normal stamp logic cannot prove that output/build/configd-* and output/target
# correspond to the just-synchronized repository source. Always rebuild this
# comparatively small package, then verify the installed target binary before
# allowing filesystem finalization. Removing the installed files also prevents
# a failed package build from being masked by an older target copy.
log "Rebuilding configd from synchronized local sources"
make configd-dirclean
rm -f output/target/bin/configd output/target/usr/bin/postmerkosctl
run_logged buildroot-configd \
  make -j"$JOBS" BR2_DL_DIR="$BUILDROOT_DL_DIR" \
  BR2_PRIMARY_SITE="https://sources.buildroot.net" configd

configd_binary="output/target/bin/configd"
postmerkosctl_binary="output/target/usr/bin/postmerkosctl"
[[ -x "$configd_binary" ]] || die "Fresh configd package build did not install /bin/configd"
[[ -x "$postmerkosctl_binary" ]] || die "Fresh configd package build did not install /usr/bin/postmerkosctl"

# Do not use grep -q in producer pipelines while pipefail is enabled. grep -q
# closes the pipe after the first match and can make strings/readelf exit with
# SIGPIPE (141), turning a successful feature check into a false failure.
if (( current_ui_mode )); then
  strings "$configd_binary" | grep -Fx 'websocket: enabled' >/dev/null || \
    die "Fresh web configd build lacks the 'websocket: enabled' feature marker"
  readelf -d "$configd_binary" | grep -F 'libwebsockets' >/dev/null || \
    die "Fresh web configd build is not linked against libwebsockets"
else
  strings "$configd_binary" | grep -Fx 'websocket: disabled' >/dev/null || \
    die "Fresh console configd build lacks the 'websocket: disabled' feature marker"
  if readelf -d "$configd_binary" | grep -F 'libwebsockets' >/dev/null; then
    die "Fresh console configd build unexpectedly links against libwebsockets"
  fi
fi
printf '%s\n' "$current_configd_fingerprint" > "$configd_fingerprint_stamp"

log "Building root filesystem and complete NOR image"
export MS42P_KERNEL_ELF="$KERNEL_ARTIFACT_DIR/vmlinuz"
export MS42P_KERNEL_BIN="$KERNEL_ARTIFACT_DIR/vmlinuz.bin"
export MS42P_LOADER="$LOADER_ARTIFACT"
export MS42P_LOADER_MANIFEST="$LOADER_MANIFEST"
export MS42P_PAYLOAD_PACKER="$LOADER_PAYLOAD_PACKER"
export MS42P_RECOVERY_ARTIFACT_DIR="$RECOVERY_ARTIFACT_DIR"
export MS42P_RELEASE="${MS42P_RELEASE:-$(date -u +%Y%m%d)}"
run_logged buildroot-build \
  make -j"$JOBS" BR2_DL_DIR="$BUILDROOT_DL_DIR" \
  BR2_PRIMARY_SITE="https://sources.buildroot.net"
ROOTFS="$BUILDROOT_DIR/output/images/rootfs.squashfs"
IMAGE="$BUILDROOT_DIR/output/images/ms42p-firmware.bin"
[[ -f "$ROOTFS" ]] || die "Buildroot did not produce rootfs.squashfs"
[[ -f "$IMAGE" ]] || die "The MS42P post-image script did not produce ms42p-firmware.bin"
[[ "$(file_size "$IMAGE")" -eq $((0x1000000)) ]] || die "Firmware image is not 16 MiB"
printf '%s\n' "$current_ui_mode" > "$ui_mode_stamp"
python3 "$VENDOR_MODULE_TOOL" verify \
  "$BUILDROOT_DIR/output/target/lib/modules" \
  --required-file "$VENDOR_MODULE_REQUIRED" --quiet || \
  die "Built target does not contain the complete platform module matrix"


stamp="$(date -u +%Y%m%d-%H%M%S)"
name="ms42p-postmerkos-$stamp.bin"
if bool_enabled "${INCLUDE_UI:-0}"; then
  name="ms42p-postmerkos-webui-$stamp.bin"
fi
cp -f "$IMAGE" "$ARTIFACTS_DIR/$name"
cp -f "$ROOTFS" "$ARTIFACTS_DIR/rootfs.squashfs"

release_manifest="$BUILDROOT_DIR/output/images/postmerkos-release.json"
if [[ -f "$release_manifest" ]]; then
  python3 "$SCRIPT_DIR/write-artifact-manifest.py" \
    "$release_manifest" \
    "$ARTIFACTS_DIR/$name.manifest.json" \
    "$ARTIFACTS_DIR/$name" \
    "$ARTIFACTS_DIR/rootfs.squashfs" \
    "$LOADER_MANIFEST" \
    "$RECOVERY_ARTIFACT_DIR" \
    "$LOADER_SOURCE_VERSION_FILE" \
    "$LOADER_SOURCE_REVISION_FILE"
  write_sha256_sidecar "$ARTIFACTS_DIR/$name.manifest.json"
else
  warn "Release manifest was not produced; modern artifact manifest sidecar omitted."
fi
write_sha256_sidecar "$ARTIFACTS_DIR/$name"
write_sha256_sidecar "$ARTIFACTS_DIR/rootfs.squashfs"
printf '%s\n' "$ARTIFACTS_DIR/$name" > "$ARTIFACTS_DIR/latest-image.txt"
touch "$STAMP_DIR/rootfs-built"
log "Created $ARTIFACTS_DIR/$name"
