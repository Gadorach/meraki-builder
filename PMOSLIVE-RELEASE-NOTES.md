# PMOSLIVE mem=120M hardware-test source release

Date: 2026-07-18
Status: source-complete and structurally validated; hardware validation and the production GCC 4.7.3/full Buildroot image build remain pending.

## Resolved hold items

1. The legacy MIPS boot parameter workspace moved from uncached `0xa0001000`
   (physical `0x1000`) to uncached `0xa0000400` (physical `0x400`). The workspace
   is bounded to 3 KiB and ends at physical `0xfff`. Physical `0x000-0x3ff`
   remains reserved for exception vectors, and the decompressed kernel begins at
   physical `0x1000`.
2. PMOSLIVE now passes `mem=120M`. The external SquashFS initrd occupies physical
   112-120 MiB and remains inside Linux-managed memory. Only physical 120-128 MiB
   is excluded.

Runtime checks reject boot-argument growth beyond the low-page workspace and
emit `PMOSLIVE RESULT ERROR BOOT-PARAMS-OVERFLOW` rather than entering Linux.

## Additional boot-region correction

A complete wrapper build showed that duplicating the 117,832-byte stage in both
active and fallback loaders exceeded the 256 KiB boot region. The wrapper now
contains:

- independent active and fallback compact LinuxLoader bodies below `0x20000`;
- one shared immutable stage-1 blob at flash offset `0x20000`;
- padding after the shared stage through the end of the 256 KiB region.

Both loader bodies derive the boot-region base from their runtime address and
copy the same shared stage to uncached RAM at `0xa7f00000`.

## Structural artifact sizes

- PMOSLIVE Jaguar1: 27,352 bytes
- Shared UART stage with both PMOSREC families and PMOSLIVE: 117,832 bytes
- Compact LinuxLoader body: 16,408 bytes
- Complete dual-loader boot region: 262,144 bytes

The binaries under `structural-artifacts/` were produced with Clang/LLD as MIPS32r2
structural builds. They are not substitutes for the pinned production GCC 4.7.3
and complete retail meraki-builder build.

## Production-build limitation

The referenced `project-tools-2.7z.001` through `.020` volumes were not exposed as
container files in this session. Therefore the pinned GCC toolchain, Linux
3.18.123 rebuild, Buildroot rootfs rebuild, and final 16 MiB retail image were not
produced here. The complete modified source trees and deterministic structural
artifacts are included for the next production build.
