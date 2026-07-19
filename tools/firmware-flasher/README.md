# Firmware flasher

`firmware-flasher.sh` validates a selected artifact and starts the normal Linux
updater, meraki-redboot pre-kernel UART recovery, or non-destructive PMOSLIVE RAM
boot. The interactive operation menu exposes all three workflows.

## Artifact contracts

Manifest-aware mode requires the image, image SHA-256, JSON release manifest,
and manifest SHA-256. `--checksum-only` uses the current Linux updater with an
image and SHA-256 sidecar. `--legacy` targets direct `fw_update_tftp` compatibility
installations and cannot perform a complete-NOR update.

## Linux updater mode

The flasher can publish an image through a private read-only TFTP server and
control the target over SSH or hardware serial. Linux UART transport is
available with `--control serial --transport uart`. System scope updates the
normal system regions; full scope requires an exact 16 MiB image and the
independent full-flash acknowledgements.

## PMOSLIVE RAM boot

The main flasher performs local capability checks, target-side validation-only
runs, and complete RAM boots:

```sh
# Local image/loader/kernel/rootfs/payload validation; no serial access.
./tools/firmware-flasher/firmware-flasher.sh \
  --firmware artifacts/<full-image>.bin \
  --liveboot-verify \
  --target-model MS42P

# Transfer and parse everything on the target, but do not enter Linux.
./tools/firmware-flasher/firmware-flasher.sh \
  --firmware artifacts/<full-image>.bin \
  --liveboot-dry-run \
  --liveboot-path ram-upload \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>

# Use installed menu option 3 and boot Linux entirely from RAM.
./tools/firmware-flasher/firmware-flasher.sh \
  --firmware artifacts/<full-image>.bin \
  --liveboot \
  --liveboot-path embedded \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>
```

`--liveboot-path` accepts `embedded`, `ram-upload`, or `auto`. RAM upload uses
`artifacts/liveboot/pmoslive-<soc-family>.bin` by default; override it with
`--liveboot-payload` and `--liveboot-descriptor`. The RAM-upload path accepts
both the legacy two-option RedBoot menu and the newer three-option menu. In
`auto` mode, a legacy menu is detected in place and option 1 is selected
without requiring another reset; option 3 is used only when it is advertised.

Live capability is a separate, stricter contract. Images without
`recovery.uart_liveboot` metadata remain eligible for ordinary SSH, serial, and
bootloader recovery flashing; only a requested PMOSLIVE operation rejects them.
The serial runner understands menu option 3, menu-option-1 payload upload,
PMOSREC v3 image transfer, `BOOTRAM <nonce>`, UART restoration to 115200, and
strict kernel and userspace evidence that the transferred RAM root is active.
A generic Linux banner or normal postmerkOS console is not accepted as live-boot success.

See [`PMOSLIVE.md`](PMOSLIVE.md) for the RAM map and safety boundary.

## Pre-kernel firmware recovery

```sh
./tools/firmware-flasher/firmware-flasher.sh \
  --bootloader-recovery \
  --firmware artifacts/<full-image>.bin \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>
```

The default RAM-upload path keeps bootloader and executable transfer at the
stable 115200 baud, then runs PMOSREC v3 from RAM. By default the host tries
921600, 460800, and 230400 baud once each, fastest first, and stops at the first
bidirectional pass. It then qualifies 4096-byte frames with a flow-control-safe
one-frame compact-ACK window, sparse reconstruction and LZ4 blocks. It
validates the manifest before the image and
selects the smallest qualified wire representation.

The wrapper automatically returns the target's live erase challenge only after
the user has supplied `FLASH-ALL`. `--manual-target-confirmation` keeps the
target waiting indefinitely and allows unlimited retries. Successful flashing
ends with a target-side five-second reset countdown. On `PMOSREC REBOOT NOW`,
the host automatically returns the serial adapter to 115200 baud and monitors
the normal loader boot.

Use `--diagnostic-window-scan` only for engineering tests of paced multi-frame
windows; normal flashing deliberately remains at window 1.

Use `--bootloader-preflight` to qualify UART and destructive-but-restored SPI
NOR behavior without transferring a firmware image. Detailed behavior is in
[`BOOTLOADER-RECOVERY.md`](BOOTLOADER-RECOVERY.md).

## Examples

```sh
./tools/firmware-flasher/firmware-flasher.sh
./tools/firmware-flasher/firmware-flasher.sh --checksum-only --version 2026.06.20
./tools/firmware-flasher/firmware-flasher.sh --control serial --transport uart
./tools/firmware-flasher/firmware-flasher.sh --bootloader-recovery --target-model MS42P
./tools/firmware-flasher/firmware-flasher.sh --bootloader-recovery --target-model MS220-8P
./tools/firmware-flasher/firmware-flasher.sh --bootloader-recovery --recovery-path ram-upload \
  --recovery-payload artifacts/recovery/recovery-jaguar1.bin --target-model MS42P
./tools/firmware-flasher/firmware-flasher.sh --liveboot-verify \
  --firmware artifacts/<full-image>.bin --target-model MS42P
./tools/firmware-flasher/firmware-flasher.sh --liveboot-dry-run \
  --firmware artifacts/<full-image>.bin --liveboot-path ram-upload \
  --target-model MS42P --serial-device /dev/serial/by-id/<adapter>
./tools/firmware-flasher/firmware-flasher.sh --liveboot \
  --firmware artifacts/<full-image>.bin --liveboot-path embedded \
  --target-model MS42P --serial-device /dev/serial/by-id/<adapter>
./tools/firmware-flasher/firmware-flasher.sh --self-test
```

## Control-flow diagnostics

Optional helper guards return success when their optional work is not required, so selecting `system` or `full` scope continues into validation rather than triggering shell `set -e`. An ERR trap prints the failing command, status, and source line for unexpected termination. Run:

```sh
./tests/test_flasher_control_flow.sh
```
