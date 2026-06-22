# meraki-redboot UART firmware recovery

The flasher supports pre-kernel recovery without Linux, networking, SSH or a
working root filesystem.

## Recovery entry paths

- **RAM upload, menu option 1:** uploads the current family-specific PMOSREC
  executable to `0x81000000`, verifies it through `PMOSRAM2`, and executes it.
  This is the default because it always supplies the latest recovery logic.
- **Embedded recovery, menu option 2:** executes the PMOSREC payload stored in
  the installed loader. Use it only when the installed loader advertises the
  same PMOSREC v3 contracts as the host and selected image.
- **Automatic:** attempts embedded recovery and falls back to RAM upload after a
  power cycle when the embedded stage cannot satisfy the current contract.

The RAM-loader bootstrap always remains at 115200 baud. Adaptive rate testing
starts only after PMOSREC v3 is running in RAM.

## Rebuild requirement

The selected recovery descriptor and final firmware manifest must declare:

```text
format: postmerkos.uart-recovery-payload.v3
protocol_version: 3
entry_contract: flat-binary-byte-zero-v1
manifest_lookup_contract: direct-object-members-v1
hardware_preflight_contract: spi-nor-scratch-rw-restore-loader-crc-v4
adaptive_transport_contract: pmosrec-v3-adaptive-uart-sparse-lz4-v1
```

Rebuild stale loader and recovery artifacts with:

```sh
REBUILD_LOADER=1 make all
```

## Normal interactive full flash

Run:

```sh
./tools/firmware-flasher/firmware-flasher.sh
```

Select a complete 16 MiB artifact, full scope, flash or force flash,
meraki-redboot UART recovery and RAM upload. The wrapper obtains `FLASH-ALL`
authorization before opening the destructive path.

Equivalent command-line use:

```sh
./tools/firmware-flasher/firmware-flasher.sh \
  --bootloader-recovery \
  --recovery-path ram-upload \
  --firmware artifacts/<full-image>.bin \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>
```

## PMOSREC v3 sequence

1. Enter the boot menu at 115200 baud.
2. Upload and verify the recovery executable through `PMOSRAM2` when using
   menu option 1.
3. Require `PMOSREC READY 3`, the complete `PMOSRECOVERY3` descriptor, UART
   capability record and early SPI NOR preflight.
4. Try conventional UART rates 921600, 460800, and 230400 once each, fastest
   first, with deterministic bidirectional CRC-32 streams.
5. Select the first passing rate; independently roll back after each failure.
6. Qualify 4096-byte framing with the flow-control-safe one-frame compact-ACK
   window, then qualify sparse reconstruction and LZ4 blocks. Fall back to
   1024-byte frames if necessary.
7. Transfer and validate the manifest before the firmware object.
8. Select the smallest qualified raw, sparse, LZ4 or sparse-LZ4
   representation.
9. Reconstruct the complete 16 MiB image and verify its CRC-32 and SHA-256.
10. Automatically return the live target challenge under the wrapper's prior
    `FLASH-ALL` authorization, or wait indefinitely in manual mode.
11. Erase, program and read back the NOR.
12. Reboot automatically after a five-second target-side countdown.

## Manual target confirmation

Use:

```sh
--manual-target-confirmation
```

The complete command must be entered, including the challenge:

```text
ERASEFLASH 12620a82
```

Incorrect input does not cancel recovery. PMOSREC repeats the expected command
and waits forever. Power cycle the switch to cancel without writing flash.

## Speed and diagnostic options

- `--skip-baud-negotiation` keeps PMOSREC at 115200 baud while retaining v3
  framing and integrity checks.
- `--diagnostic-baud-scan` enables the legacy broad divisor scan and midpoint
  refinement for engineering diagnostics; it is intentionally not the default.
- `--diagnostic-window-scan` tests paced windows 1, 2, 4, 8 and 16 in ascending
  order. The host drains its TTY output queue and inserts a 3 ms wire-idle guard
  between frames. Normal flashing uses window 1 because PMOSREC has no RTS/CTS.
- `--verbose-acks` prints each decoded compact acknowledgement in addition to
  normal progress.
- USB-serial latency is reduced to 1 ms where the Linux driver exposes a
  writable latency timer, then restored at exit.

Initial ETA is derived from the selected baud and wire representation. It is
replaced by rolling measured throughput once enough data has transferred.

## Console-output safety

The recovery client prints only complete target protocol lines. Binary compact
ACKs and deterministic target-to-host baud-test payloads are never copied to the
operator terminal or text log. A serial read can contain an ASCII header and the
start of its binary payload in the same kernel buffer, so raw-read echoing is not
safe: random payload bytes can include XOFF or terminal escape sequences that
make a healthy transfer appear stalled.

When using an older client that has already stopped rendering while its log keeps
growing, press `Ctrl+Q` once to release terminal flow control without aborting the
flash. The corrected client does not require this workaround.

## Hardware preflight

Run the short destructive-but-restored test before a full flash:

```sh
./tools/firmware-flasher/firmware-flasher.sh \
  --bootloader-preflight \
  --recovery-path ram-upload \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>
```

The preflight performs the adaptive UART tests, JEDEC/SFDP/status checks and a
complete erase/program/readback/restore cycle on one 64 KiB scratch sector. It
also checks the entire 256 KiB bootloader region before and after the test.
The default scratch sector is `0x00ff0000`; addresses below `0x00040000` are
rejected.

## Safety

Pre-kernel full flash overwrites loader, kernel, SquashFS and JFFS2. Keep a
verified external SPI backup and programmer available. The target does not
begin erase until all transport, manifest, reconstructed-image and hardware
checks have passed and the exact live challenge has been authorized.

After verified programming, PMOSREC requests the family soft-chip reset and arms the family-specific ICPU watchdog if execution unexpectedly continues.
