# Firmware flasher

`firmware-flasher.sh` validates a selected artifact and starts either the normal
Linux updater or meraki-redboot pre-kernel UART recovery.

## Artifact contracts

Manifest-aware mode requires the image, image SHA-256, JSON release manifest,
and manifest SHA-256. `--checksum-only` uses the current Linux updater with an
image and SHA-256 sidecar. `--legacy` targets older direct `fw_update_tftp`
installations and cannot perform a complete-NOR update.

## Linux updater mode

The flasher can publish an image through a private read-only TFTP server and
control the target over SSH or hardware serial. Linux UART transport is
available with `--control serial --transport uart`. System scope updates the
normal system regions; full scope requires an exact 16 MiB image and the
independent full-flash acknowledgements.

## Pre-kernel mode

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
./tools/firmware-flasher/firmware-flasher.sh --self-test
```

## Silent exit after selecting flash scope

A previous revision could return directly to the shell after selecting either
`system` or `full`. The flasher runs with `set -e`, and several optional helper
functions used a bare `return` after a failed guard expression. In modern mode,
`prepare_version_hint` consequently returned status 1 even though skipping the
checksum-only version prompt was expected.

All optional no-op guards now return status 0 explicitly. An ERR trap also prints
the failing command, status, and source line for any future unexpected `set -e`
termination. The regression test is:

```sh
./tests/test_flasher_control_flow.sh
```
