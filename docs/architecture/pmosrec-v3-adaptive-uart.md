# PMOSREC v3 adaptive UART transport

PMOSREC v3 is the pre-kernel firmware recovery protocol launched by
meraki-redboot. The permanent bootloader menu and `PMOSRAM2` executable upload
remain fixed at 115200 baud. Only the RAM-resident PMOSREC stage negotiates
faster transport parameters.

## Stable bootstrap

The host always performs these steps at 115200 baud:

1. enter the meraki-redboot recovery menu;
2. select menu option 1 or corrected menu option 2;
3. upload and verify the family-specific recovery executable when option 1 is
   used;
4. execute PMOSREC and validate its complete descriptor;
5. complete JEDEC, SFDP, status and protection preflight.

A reset returns the target to this stable path regardless of any later adaptive
transport result.

## Baud qualification

The host proposes a rate. PMOSREC calculates the nearest rate that the target
UART can generate from its current clock and integer divisor and reports both
the requested and actual values. The host attempts to configure that exact
actual rate.

Before every switch, both sides record the current known-good rate and a random
nonce. They independently arm rollback behavior. At the candidate rate they
exchange synchronization records and run two bidirectional deterministic
pseudorandom tests. Each side already knows the expected stream CRC-32.

A candidate passes only when:

- host-to-target data matches the deterministic stream and CRC-32;
- target-to-host data matches the deterministic stream and CRC-32;
- the target reports no UART line-status errors;
- both test passes complete;
- the host commits the candidate rate.

On any failure, the target independently returns to the previous divisor and
emits repeated fallback beacons. The host independently restores the previous
rate and waits for the fallback-ready marker. No recovery message has to cross
the failed baud rate.

The normal host policy tries 921600, 460800, and 230400 baud once each, fastest
first, and stops at the first passing candidate. This bounds negotiation time and
avoids speculative nonstandard divisors during ordinary recovery. The target
still accepts arbitrary proposals, and `--diagnostic-baud-scan` retains the broad
rate sweep and midpoint refinement for engineering work.

## Framing and windows

Preferred production framing is:

- 4096-byte decoded frames;
- a one-frame production window;
- binary cumulative acknowledgements;
- a selective retry bitmap;
- CRC-32 for the frame header, wire payload and decoded payload.

If 4096-byte framing fails qualification, the host falls back to 1024-byte
frames. A failed frame retransmits only that frame. The production window stays
at one because the recovery UART has no RTS/CTS flow control: after each frame,
the target must CRC, decode and copy data before it can safely receive another
header. A continuous multi-frame USB-serial burst can overrun the target FIFO at
higher baud rates.

`--diagnostic-window-scan` retains ascending 1, 2, 4, 8 and 16-frame tests.
Diagnostic multi-frame transfers wait for the host TTY output queue to drain and
insert a conservative wire-idle guard between frames. The first failing larger
window ends the scan and leaves the last passing value selected.

The compact acknowledgement retains object ID, window base, frame count,
selective retry bitmap, UART/retry status and its own CRC-32. The target
transmits the record
through a byte-transparent UART writer, and the host scans for ACK magic so it can
recover alignment after a damaged or shortened record. If no ACK is recovered,
non-ACK target output is restored to the line parser so a structured
`FEATURE-FAIL` remains visible. The host confirms each valid acknowledgement.
PMOSREC also recognizes the first byte of the next frame
if a one-byte ACK confirmation is lost and preserves it in a pushback slot.

## Feature qualification

At the selected baud rate the host sends a small synthetic object through the
same production framing. It qualifies:

- raw transfer;
- sparse `0xff` reconstruction;
- independent LZ4 blocks;
- combined sparse plus LZ4 reconstruction.

Any configuration that requires a frame retransmission or reports UART receive
status during qualification is not selected for production use. A failed
optional representation is disabled
without ending recovery. Raw framed transfer remains the final fallback.

## Manifest-first package order

The JSON release manifest is transferred and validated before the firmware
payload. PMOSREC checks the exact model, SoC family, flash geometry, loader and
recovery contracts, image size and declared digest before accepting the large
object.

The host then calculates the wire size of every qualified representation:

- raw;
- sparse;
- LZ4;
- sparse plus LZ4.

The smallest qualified representation is selected. PMOSREC reconstructs the
complete 16 MiB image in RAM, computes CRC-32 and SHA-256 over the reconstructed
image, and compares them with the authoritative manifest and package header.
No representation can alter the identity of the image that is eventually
flashed.

## Progress and ETA

Progress rendering is host-side. PMOSREC does not emit extra per-frame text.
The host decodes compact acknowledgements, logs protocol state and calculates:

- acknowledged wire bytes;
- initial ETA from negotiated baud rate and selected wire size;
- rolling measured throughput after the transfer begins;
- retransmission count;
- erase, program and readback phase progress.

The default terminal view emphasizes current progress. `--verbose-acks` adds
per-window acknowledgement diagnostics. The complete recovery log remains
available under `logs/`.

## Erase authorization and reboot

The wrapper still requires the user's full-flash authorization before serial
recovery begins. After PMOSREC validates the manifest and reconstructed image,
it generates a live `ERASEFLASH <nonce>` challenge.

Normal wrapper use automatically returns the exact live challenge because the
user already supplied `FLASH-ALL`. Direct use without the wrapper must select
manual confirmation or explicitly supply the hidden authorization state.

Manual confirmation retries forever. Incorrect text does not discard the image
or terminate the target session. Power cycling is the cancellation mechanism.

After erase, program and full readback verification succeed, PMOSREC emits a
five-second countdown and requests the SoC soft-chip reset. `PMOSREC REBOOT NOW`
is the explicit baud handoff marker: the host consumes that final line at the
negotiated transport rate, immediately restores its UART to 115200 baud, clears
only stale high-speed receive bytes, and then watches for the next loader or
kernel banner. If execution continues, PMOSREC arms the family-specific ICPU
watchdog as a last-resort reset.

## Integrity boundaries

Adaptive transport retains all existing safety checks:

- frame-header CRC-32;
- wire-payload CRC-32;
- decoded-payload CRC-32;
- compact-ACK CRC-32;
- object CRC-32 and SHA-256;
- reconstructed full-image SHA-256;
- direct-member manifest parsing;
- exact model and SoC-family binding;
- JEDEC/SFDP/status/protection validation;
- bootloader region protection;
- flash readback verification.

The machine-readable contract is:

```text
PMOSRECOVERY3
PROTO=3
PREFLIGHT=4
adaptive_transport_contract=pmosrec-v3-adaptive-uart-sparse-lz4-v1
hardware_preflight_contract=spi-nor-scratch-rw-restore-loader-crc-v4
```
