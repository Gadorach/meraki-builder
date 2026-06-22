# Pre-kernel UART recovery

The pre-kernel path is supplied by the source-built meraki-redboot boot region.
It does not require Linux, networking, SSH or a working root filesystem.

| Path | Bootstrap | Firmware transport | Flash engine |
|---|---|---|---|
| Normal UART update | Linux userspace | `PMOSUART/1` | Linux MTD updater |
| Embedded recovery | menu option 2 at 115200 | PMOSREC v3 adaptive binary transport | embedded family recovery stage |
| RAM-upload recovery | `PMOSRAM2` at 115200 | PMOSREC v3 adaptive binary transport | freshly uploaded family recovery stage |

## Boot menu and stable recovery boundary

Stage 1 reports `PMOSBOOT MENU-PROBE`. The host sends carriage return as the
menu trigger and selects:

1. `UART-RAMLOADER` for a fresh executable;
2. `FW-RECOVERY` for the payload embedded in the loader.

No input, invalid input or menu timeout continues normal boot. The RAM loader
validates load range, entry address, frame CRC-32, object CRC-32 and SHA-256
before executing the payload. This bootstrap always uses 115200 baud.

## PMOSREC v3

The current stage reports:

```text
PMOSREC READY 3 SOC=<family>
PMOSREC DESCRIPTOR PMOSRECOVERY3;...;PROTO=3;PREFLIGHT=4;...;END
PMOSREC UART-CAP CLOCK=<hz> DIV_MIN=1 DIV_MAX=65535 CURRENT=115200
```

The host waits for the complete descriptor and early SPI NOR preflight before
sending commands. PMOSREC then negotiates the fastest target-generated UART
rate that passes deterministic bidirectional CRC testing. Every failed switch
rolls back independently to the previous known-good rate.

Production transfer prefers 4096-byte frames with one frame in flight.
Compact CRC-protected acknowledgements carry a cumulative window and selective
retry bitmap. Production uses a one-frame window so the target can complete
CRC/decode/copy work before the next frame arrives without hardware flow
control. Larger paced windows are diagnostic-only.

The manifest is sent and validated first. The host then chooses the smallest
qualified representation among raw, sparse, LZ4 and sparse-LZ4. PMOSREC
reconstructs the exact 16 MiB image in RAM and verifies its complete SHA-256
before an erase challenge can be issued.

See [PMOSREC v3 adaptive UART transport](pmosrec-v3-adaptive-uart.md) for the
wire protocol, rollback and integrity details.

## Destructive authorization

The wrapper requires full-image authorization before invoking PMOSREC. After
all target validation succeeds, PMOSREC emits a live `ERASEFLASH <nonce>`
challenge. The wrapper responds automatically under the prior `FLASH-ALL`
authorization. Manual diagnostic mode waits forever and allows unlimited
incorrect retries; power cycling cancels.

After full erase, program and readback verification, PMOSREC performs a
five-second countdown and requests the SoC soft-chip reset.

## Family naming boundary

The boot/recovery layer calls MS42/MS42P hardware `jaguar1` because it selects
the Jaguar1 SPI register map. The Linux runtime remains `jaguar_dual` and loads
the donor `jaguar_dual` switch modules. These namespaces are intentionally
separate.
