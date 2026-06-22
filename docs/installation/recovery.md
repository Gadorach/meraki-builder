# Recovery

Keep at least two matching full-device SPI backups before testing firmware or
updater changes.

## Linux-level recovery

Use the normal updater, backup/restore, or factory-reset functions when Linux
boots and the management console is available. A configuration reset affects
JFFS2-backed settings but does not replace meraki-redboot, the kernel, or the
SquashFS region.

## meraki-redboot embedded UART recovery

Use this path when the 256 KiB boot region starts but Linux or the normal updater
cannot run. Requirements:

- a postmerkOS image built with meraki-redboot manifest format v7;
- the exact 16 MiB image and matching release manifest;
- exact target model selection;
- 3.3 V UART at 115200 8N1;
- a verified external SPI backup and programmer.

```sh
./tools/firmware-flasher/firmware-flasher.sh \
  --bootloader-recovery \
  --firmware artifacts/<full-image>.bin \
  --target-model MS42P \
  --serial-device /dev/ttyUSB0
```

The flasher triggers `PMOSBOOT MENU-PROBE`, uses menu option 1 by default to upload the corrected family recovery stage.
Menu option 2 is used only after a loader with the byte-zero entry correction is
installed. Local verification includes the loader source capability, model
family, embedded recovery digest, image SHA-256, SPIM alignment and CRC, and
flash geometry before serial transfer.

Use `--recovery-path ram-upload --recovery-payload FILE` only for deliberate
option-1 testing or compatibility with a pre-menu/direct RAM-loader. The
external payload must match the image manifest exactly.

- **Verify**: no serial access or writes.
- **Dry run**: target validation and flash preflight; no erase/program.
- **Flash**: full 16 MiB erase/program/readback after host and target nonce
  authorization.

## External SPI recovery

Use an external programmer when meraki-redboot does not start, the loader region
is corrupt, flash identification or protection checks fail, or a write is
interrupted.

1. Remove switch power and disconnect Ethernet/SFP connections.
2. Attach the SPI programmer with the switch unpowered.
3. Read the complete device at least twice and compare SHA-256 values.
4. Retain those reads before writing anything.
5. Write an exact 16 MiB backup or a validated complete postmerkOS image.
6. Read the full device back and compare it byte-for-byte with the source.
7. Electrically release or disconnect the programmer before powering the switch.
