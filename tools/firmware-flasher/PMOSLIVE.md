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

The builder applies the managed VCore-III kernel patch and enforces standard
MIPS/U-Boot firmware arguments, built-in legacy initrd/RAM-disk support, and
SquashFS-XZ support:

```text
CONFIG_CMDLINE_BOOL=y
# CONFIG_CMDLINE_OVERRIDE is not set
CONFIG_CMDLINE_FALLBACK=y
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

The VCore-III PROM path imports the standard 32-bit MIPS `a0=argc`, `a1=argv`,
`a2=envp`, `a3=extra` convention used by U-Boot. Command-line arguments are
preferred whenever a valid vector is supplied. U-Boot legacy environment keys
`memsize`, `initrd_start`, and `initrd_size` are normalized into `mem=`,
`rd_start=`, and `rd_size=` when those arguments are absent. When an old
RedBoot supplies no usable vector, the existing compiled SPI-flash command line
is used as a fallback; it is never appended after valid firmware arguments.

At live boot, the normal SquashFS is supplied as `/dev/ram0`. Early userspace
sees `postmerkos.live=1`, mounts the writable overlay on tmpfs, does not mount
the persistent JFFS2 overlay, and emits:

```text
PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0
```

## Safety boundary

The PMOSLIVE linked image contains no SPI NOR erase/program implementation or
flash preflight path. The live rootfs also rejects normal firmware-update and
factory-reset writes. Raw privileged MTD access from a root shell is outside
that userspace policy, so development images should still be treated as
trusted code.

## Main firmware-flasher integration

PMOSLIVE is exposed by the normal firmware utility, both interactively and by
command line. The utility labels manifest-aware full images as `manifest+live` only when they
declare the complete PMOSLIVE contract **and** the packaged SPIM payload is
bound to a verified kernel build record for the standard MIPS/U-Boot argument
patch. Older images remain valid for ordinary flashing but are not offered for
live boot.

```sh
# Host-only strict capability validation.
./tools/firmware-flasher/firmware-flasher.sh \
  --firmware artifacts/<full-image>.bin \
  --liveboot-verify --target-model MS42P

# Full transfer and target parsing without entering Linux.
./tools/firmware-flasher/firmware-flasher.sh \
  --firmware artifacts/<full-image>.bin \
  --liveboot-dry-run --liveboot-path ram-upload \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>

# Boot using the PMOSLIVE payload embedded behind menu option 3.
./tools/firmware-flasher/firmware-flasher.sh \
  --firmware artifacts/<full-image>.bin \
  --liveboot --liveboot-path embedded \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>
```

The path can be `embedded`, `ram-upload`, or `auto`. A ram-upload/auto run uses
`artifacts/liveboot/pmoslive-jaguar1.bin` and its adjacent descriptor unless
`--liveboot-payload` and `--liveboot-descriptor` override them. `ram-upload` is
compatible with older RedBoot builds whose menu contains only options 1 and 2.
`auto` inspects the advertised menu: it selects embedded option 3 when present,
or immediately selects option 1 and uploads PMOSLIVE when the installed loader
has the legacy two-option menu.

Capability gating is intentionally asymmetric: PMOSLIVE operations require the
live metadata, compatible kernel/rootfs handoff, and flash-disabled payload.
Ordinary firmware flashing does **not** require those records and continues to
accept otherwise valid non-live images.

The integrated serial interpreter handles the new boot-menu choice, PMOSRAM2
payload upload where selected, PMOSLIVE readiness and RAM-map records, PMOSREC
v3 manifest/image transfer, the non-destructive `BOOTRAM <nonce>` challenge,
UART restoration to 115200, and strict proof of the complete Linux handoff. A
Linux banner alone is not success. The interpreter requires VCore-III argv
acceptance, firmware command-line selection, initrd reservation, the complete
live command line, a non-MTD SquashFS root mount, and the userspace attestation
above. It rejects the observed failure signatures `Initrd not found or empty`,
`root=/dev/mtdblock3`, `mem=134152192`, and root device `31:3`.

## Validation and first test

Run host validation first, then a serial dry run, then a real boot. A power
cycle always abandons a RAM-only operation and returns to the installed
loader/firmware.

The first hardware run can add `--skip-baud-negotiation` if maximum simplicity
is preferred. After a successful dry run at 115200, repeat with the normal
adaptive baud path. A successful boot must include at least:

```text
VCOREIII PROM ARGV-ACCEPTED ...
MIPS CMDLINE-SOURCE=firmware
Kernel command line: ... mem=120M ... root=/dev/ram0 ... postmerkos.live=1
Initial ramdisk at: ...
VFS: Mounted root (squashfs filesystem) ...
PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0
```

Kernel artifacts are reusable only when
`artifacts/kernel/pmoslive-kernel-contract.json` verifies the source revision,
managed patch, config policy, and hashes of `vmlinuz`, `vmlinuz.bin`, and the
kernel-header archive. The release-manifest writer also verifies that the SPIM
payload matches this record before publishing live capability.
