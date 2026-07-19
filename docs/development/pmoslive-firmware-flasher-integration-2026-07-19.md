# PMOSLIVE firmware-flasher integration — 2026-07-19

The normal `tools/firmware-flasher/firmware-flasher.sh` interface now exposes
PMOSLIVE alongside the Linux updater and pre-kernel firmware recovery.

## Operator interface

Interactive operation choices include local live-capability verification,
target-side PMOSLIVE dry run, and complete RAM boot. Command-line equivalents
are `--liveboot-verify`, `--liveboot-dry-run`, and `--liveboot`, with
`--liveboot-path embedded|ram-upload|auto` selecting the entry path.

The artifact list marks a modern full image as `manifest+live` only when its
manifest declares the complete PMOSLIVE contract. This is informational during
ordinary flashing.

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
- recognition of the first Linux boot banner.

The existing firmware console parser remains the default and rejects the
live-only `boot` operation.

## Validation

The integration was validated with 51 Python tests, the shell control-flow
suite, the updater host smoke test, the flasher private self-test, and strict
embedded plus RAM-upload verification against the produced
`ms42p-postmerkos-webui-20260719-141728.bin` artifact.
