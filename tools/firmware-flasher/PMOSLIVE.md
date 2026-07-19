# PMOSLIVE RAM boot for MS42/MS42P

PMOSLIVE is a Jaguar1-only, flash-write-free payload built from the proven
PMOSREC v3 UART transport. It is intended for repeated development boots of the
normal retail meraki-builder image without erasing or programming NOR.

## Runtime map

| Region | Address | Purpose |
|---|---:|---|
| Boot-parameter workspace | physical `0x00000400-0x00000fff`; uncached `0xa0000400-0xa0000fff` | compact MIPS `argv`/`envp` after the exception-vector area and below the decompressed kernel |
| Kernel | `0x81000000` | verified SPIM payload destination and entry |
| Received image | `0x81400000` | unchanged 16 MiB retail image |
| Manifest | `0x82400000` | PMOSREC v3 manifest object |
| PMOSLIVE | `0x86c00000` | UART loader executable, data, and stack |
| SquashFS initrd | `0x87000000` | verified retail rootfs, maximum 8 MiB |

Linux receives `mem=120M`. The SquashFS copy occupies physical 112-120 MiB and
is inside the declared memory so the legacy initrd handoff can reserve and
consume it. PMOSLIVE occupies physical 108-112 MiB and becomes reclaimable once
Linux has copied the command line and environment. Only physical 120-128 MiB is
excluded, protecting the fixed stage-1 reservation and leaving top-of-RAM
headroom. PMOSLIVE parses the SPIM header and CRC and the SquashFS v4
superblock, including `bytes_used`, before it allows a boot.

The 256 KiB RedBoot region keeps independent active and fallback loader bodies,
but stores their fixed-RAM UART stage only once at flash offset `0x20000`. Both
loader bodies copy that shared source-built stage to `0xa7f00000`.

## Required retail-kernel features

The builder enforces built-in legacy initrd/RAM-disk and SquashFS-XZ support:

```text
CONFIG_BLOCK=y
CONFIG_BLK_DEV=y
CONFIG_BLK_DEV_INITRD=y
CONFIG_BLK_DEV_RAM=y
CONFIG_BLK_DEV_RAM_COUNT=1
CONFIG_BLK_DEV_RAM_SIZE=16384
CONFIG_RD_XZ=y
CONFIG_SQUASHFS=y
CONFIG_SQUASHFS_XZ=y
CONFIG_XZ_DEC=y
CONFIG_DECOMPRESS_XZ=y
```

At boot, the normal SquashFS is supplied as `/dev/ram0`. Early userspace sees
`postmerkos.live=1`, mounts the writable overlay on tmpfs, and does not mount the
persistent JFFS2 overlay.

## Safety boundary

The PMOSLIVE linked image contains no SPI NOR erase/program implementation or
flash preflight path. The live rootfs also rejects normal firmware-update and
factory-reset writes. Raw privileged MTD access from a root shell is outside
that userspace policy, so development images should still be treated as
trusted code.

## Validation and first test

Run host validation first, then a serial dry run, then a real boot. See
`BOOTLOADER-RECOVERY.md` for complete commands. A power cycle always abandons a
RAM-only operation and returns to the installed loader/firmware.

The first hardware run should use `--skip-baud-negotiation` if maximum
simplicity is preferred. After a successful dry run at 115200, repeat with the
normal adaptive baud path.
