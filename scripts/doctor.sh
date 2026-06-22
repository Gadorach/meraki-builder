#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

required=(bash git make tar xz rsync python3 sha256sum readelf unsquashfs mkfs.jffs2 file)
optional=(curl wget distrobox podman node npm)
failed=0

printf 'Repository: %s\n' "$REPO_ROOT"
printf 'Work tree:  %s\n' "$WORK_DIR"
printf 'Artifacts:  %s\n\n' "$ARTIFACTS_DIR"

for cmd in "${required[@]}"; do
  if command -v "$cmd" >/dev/null 2>&1; then
    printf '  [ok]      %s\n' "$cmd"
  else
    printf '  [missing] %s\n' "$cmd"
    failed=1
  fi
done
for cmd in "${optional[@]}"; do
  if command -v "$cmd" >/dev/null 2>&1; then
    printf '  [optional] %s\n' "$cmd"
  fi
done

printf '\nInputs and build state:\n'
[[ -d "$SWITCH_DIR/.git" ]] && printf '  [ok] kernel/OpenWrt source\n' || printf '  [missing] kernel/OpenWrt source\n'
[[ -f "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" ]] && printf '  [ok] compressed kernel\n' || printf '  [missing] compressed kernel\n'
[[ -f "$KERNEL_HEADERS_TARBALL" ]] && printf '  [ok] kernel headers archive\n' || printf '  [missing] kernel headers archive\n'
[[ -d "$BUILDROOT_DIR" ]] && printf '  [ok] Buildroot %s\n' "$BUILDROOT_VERSION" || printf '  [missing] Buildroot %s\n' "$BUILDROOT_VERSION"
[[ -d "$DONOR_ROOT/lib/modules" ]] && printf '  [ok] extracted donor modules\n' || printf '  [missing] extracted donor modules\n'
[[ -f "$LOADER_ARTIFACT" ]] && printf '  [ok] 256 KiB source-built meraki-redboot\n' || printf '  [missing] meraki-redboot loader\n'

exit "$failed"
