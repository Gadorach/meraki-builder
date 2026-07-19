# PMOSLIVE firmware-flasher integration — 2026-07-19

The normal `tools/firmware-flasher/firmware-flasher.sh` interface now exposes
PMOSLIVE alongside the Linux updater and pre-kernel firmware recovery.

## Operator interface

Interactive operation choices include local live-capability verification,
target-side PMOSLIVE dry run, and complete RAM boot. Command-line equivalents
are `--liveboot-verify`, `--liveboot-dry-run`, and `--liveboot`, with
`--liveboot-path embedded|ram-upload|auto` selecting the entry path.

The artifact list marks a modern full image as `manifest+live` only when its
manifest declares the complete PMOSLIVE contract and its SPIM payload is bound
to a verified kernel build record. This is informational during ordinary
flashing.

## Capability boundary

`bootloader_protocol.validate_bundle()` accepts `require_liveboot=False` by
default. The ordinary recovery/flash path therefore continues to accept a
valid non-live image. A PMOSLIVE operation passes `require_liveboot=True` and
then requires menu option 3, the embedded/standalone PMOSLIVE binding, the
flash-disabled contract, and the expected kernel/rootfs RAM handoff.

## Serial interpretation

`serial-runner.py --console-mode liveboot` dispatches the PMOSLIVE state
machine. It handles:

- meraki-redboot menu option 3;
- menu option 1 plus PMOSRAM2 payload upload;
- PMOSLIVE readiness, descriptor, RAM-map and transport records;
- PMOSREC v3 manifest-first image transfer;
- SPIM/SquashFS target validation and dry-run completion;
- `BOOTRAM <nonce>` confirmation;
- target and host UART restoration to 115200;
- proof that the kernel accepted firmware arguments and reserved the initrd;
- a RAM-disk SquashFS root mount and exact live-userspace attestation.

The existing firmware console parser remains the default and rejects the
live-only `boot` operation.

## Validation

The integration includes positive and negative serial transcripts, including
the observed failure where the transferred kernel used its built-in flash-root
command line. A generic Linux or postmerkOS banner is no longer sufficient for
success.

## Kernel handoff correction

Live capability now requires the managed VCore-III standard MIPS/U-Boot
argument patch, `CONFIG_CMDLINE_FALLBACK=y`, and a content-bound kernel build
record. The serial interpreter requires full RAM-root attestation instead of a
generic Linux banner. Older images remain flashable but are rejected only for
live mode.
