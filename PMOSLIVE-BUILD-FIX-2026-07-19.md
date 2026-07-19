# PMOSLIVE production-build fix

Date: 2026-07-19

## Failures reproduced from the first production build

### meraki-redboot

The pinned GCC 4.7.3 build lowered a fixed-size zeroing operation to an external
`memset` call. PMOSLIVE is linked with `-nostdlib`, so the final ELF failed with
an undefined `memset` reference. GCC also warned that `rootfs_size` could be used
uninitialised.

Corrections:

- PMOSLIVE now provides freestanding local `memcpy` and `memset` implementations.
- `rootfs_size` is initialised to zero before manifest validation.
- The final structural ELF is checked for unresolved symbols as before.

### meraki-builder

`msxx_defconfig` disables the parent block-device menu. The first live-kernel
configuration helper requested `CONFIG_BLK_DEV_RAM` and its numeric children,
but did not enable `CONFIG_BLOCK` and `CONFIG_BLK_DEV`. Linux `olddefconfig`
therefore removed the child symbols and the post-resolution verifier aborted.

Corrections:

- The PMOSLIVE kernel contract now explicitly requires `CONFIG_BLOCK=y` and
  `CONFIG_BLK_DEV=y` before enabling the RAM-disk and initrd options.
- The regression fixture begins with both parent symbols disabled, matching the
  observed production-build state.
- PMOSLIVE build documentation lists the complete parent/child configuration.

## Validation

- Complete meraki-redboot host suite: 49 tests passed.
- Clang/LLD MIPS32r2 PMOSLIVE and shared-stage structural build passed.
- PMOSLIVE size remained 27,352 bytes.
- Shared UART stage remained 117,832 bytes.
- meraki-builder kernel-configuration regression passed with disabled parents.
- meraki-builder artifact-manifest and UART protocol suites passed.

The exact GCC 4.7.3 and full Linux/Buildroot production builds still need to be
rerun in the user's Ubuntu 22.04 Distrobox, where the pinned toolchains are
already installed.

## Distrobox rebuild-variable forwarding

The automatic CachyOS-to-Ubuntu Distrobox handoff now preserves
`REBUILD_KERNEL`, `REBUILD_LOADER`, `CLEAN_KERNEL`, `CLEAN_BUILDROOT`,
`LOADER_REPO_URL`, and `LOADER_REF`. This makes the documented clean rebuild
command and local committed RedBoot workflow behave identically inside and
outside Distrobox.
