# Firmware updater architecture

Fwupdate separates transport, candidate validation, flash operations, status,
persistent result finalization, and reboot. Local files, browser upload, TFTP,
HTTP/HTTPS, SFTP, and Linux UART transport all enter the same `fw_update`
validation and MTD-writing engine.

## Candidate validation

Before a write can begin, the updater checks:

- image and manifest SHA-256;
- release metadata and target family;
- exact model compatibility status;
- flash scope and complete region geometry;
- SPIM/kernel and SquashFS layout;
- version direction and explicit force policy;
- configuration-overlay policy;
- the independent final flash acknowledgement.

A `known-incompatible` candidate is blocked. An `untested` candidate requires
`ACCEPT-UNTESTED`. `validated` and `confirmed` records use the normal path.

## Linux UART transport

`fwserialrx` implements the ASCII-safe `PMOSUART/1` protocol. A sender announces
an object label, original filename, byte count, and SHA-256, then transmits
bounded Base64 frames with sequence numbers and CRC-32. The receiver ACKs each
accepted frame, tolerates a duplicate of the most recently accepted frame,
rejects gaps or CRC failure, and verifies exact byte count and whole-object
SHA-256 before publishing the object under `/run/fwupdate/uploads`.

The firmware object is required. A release manifest is transferred when the
artifact uses the manifest-aware contract. `fw_update_uart` passes both objects
to `fw_update`; UART therefore cannot bypass model, metadata, geometry,
downgrade, overlay, or acknowledgement rules. The reconstructed objects are
RAM-backed and must fit available memory.

## Browser upload

Each authenticated browser session receives a unique upload token. The original
artifact filename is retained and the matching JSON release manifest can be
uploaded alongside the image. Starting a new session removes only stale cache
entries; objects already handed to the detached updater remain available to the
running operation.

Verification produces a ready state. A separate authenticated acknowledgement
begins destructive writing.

## Status and finalization

The updater writes machine-readable status and logs under `/run/fwupdate`,
direct serial progress, capability-aware LED state, and a bounded persistent
record under `/config/postmerkos/update-history`. Post-boot finalization compares
the installed release identity with the pending record and reports success,
interruption, failure, or an indeterminate result rather than silently assuming
success.

## Pre-kernel recovery

When Linux cannot run, the source-built loader and RAM recovery payload use a
separate binary protocol and direct SPI engine. That path is documented in
[Pre-kernel UART recovery](pre-kernel-uart-recovery.md). It requires a complete
16 MiB manifest-bound image and does not share the Linux MTD implementation.
