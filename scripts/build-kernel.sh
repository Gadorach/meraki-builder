#!/usr/bin/env bash
source "$(dirname "$0")/common.sh"

[[ -d "$SWITCH_DIR/.git" ]] || "$SCRIPT_DIR/prepare-sources.sh"

if [[ "${MS42P_IN_DISTROBOX:-0}" != 1 ]]; then
  if bool_enabled "${USE_DISTROBOX:-0}"; then
    exec "$SCRIPT_DIR/distrobox-run.sh" ./scripts/build-kernel.sh
  elif command -v pacman >/dev/null 2>&1; then
    if ask_yes_no "Build the pinned OpenWrt toolchain and Linux kernel in Ubuntu 22.04 Distrobox?" yes; then
      exec "$SCRIPT_DIR/distrobox-run.sh" ./scripts/build-kernel.sh
    fi
    warn "Continuing on the Arch/CachyOS host; this host compiler combination is unsupported."
  fi
fi

for cmd in make git tar bzip2 readelf; do need "$cmd"; done

log "Building the OpenWrt cross-toolchain"
cd "$OPENWRT_DIR"
cp -f config-elemental-3.18 .config
make oldconfig
run_logged openwrt-build make -j"${OPENWRT_JOBS:-1}" \
  BOARD=elemental-3.18 OPENWRT_EXTRA_BOARD_SUFFIX=_3.18
[[ -x "${CROSS_COMPILE}gcc" ]] || die "Cross-compiler was not produced at ${CROSS_COMPILE}gcc"

log "Building Linux 3.18 compressed vmlinuz"
cd "$KERNEL_DIR"
if bool_enabled "${CLEAN_KERNEL:-0}"; then
  make ARCH=mips CROSS_COMPILE="$CROSS_COMPILE" mrproper
fi
make ARCH=mips CROSS_COMPILE="$CROSS_COMPILE" msxx_defconfig
make ARCH=mips CROSS_COMPILE="$CROSS_COMPILE" prepare
run_logged kernel-build make -j"$JOBS" ARCH=mips CROSS_COMPILE="$CROSS_COMPILE" vmlinuz
"${CROSS_COMPILE}objcopy" -O binary -S vmlinuz vmlinuz.bin

entry="$(readelf -h vmlinuz | awk '/Entry point address/ {print $4}')"
[[ "$entry" == 0x81000000 ]] || die "Unexpected vmlinuz entry point: $entry"

mkdir -p "$KERNEL_ARTIFACT_DIR"
cp -f vmlinuz vmlinuz.bin "$KERNEL_ARTIFACT_DIR/"

log "Creating Buildroot kernel-header input archive"
rm -f "$KERNEL_HEADERS_TARBALL"
tar -C "$SWITCH_DIR" -cjf "$KERNEL_HEADERS_TARBALL" \
  --transform='s|^linux-3.18|linux-3.18.123|' linux-3.18

sha256sum "$KERNEL_ARTIFACT_DIR/vmlinuz" "$KERNEL_ARTIFACT_DIR/vmlinuz.bin" \
  "$KERNEL_HEADERS_TARBALL" > "$KERNEL_ARTIFACT_DIR/SHA256SUMS"
touch "$STAMP_DIR/kernel-built"
log "Kernel artifacts are ready in $KERNEL_ARTIFACT_DIR"
