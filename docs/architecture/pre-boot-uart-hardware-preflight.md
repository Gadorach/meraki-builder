# Pre-boot UART hardware preflight

The preflight validates the complete recovery path before a full firmware
transfer or flash operation. It uploads only the small PMOSREC executable, then
tests adaptive UART transport and SPI NOR read/write behavior.

## Sequence

1. Enter meraki-redboot menu option 1 or a contract-compatible option 2.
2. Validate the PMOSREC v3 descriptor and target family.
3. Complete early SPI-controller, JEDEC, SFDP, status and protection checks.
4. Try the conventional 921600, 460800, and 230400 UART rates once each and run deterministic bidirectional
   CRC tests with independent rollback.
5. Qualify 4096-byte frames, the safe one-frame window, compact ACKs, sparse reconstruction
   and independent LZ4 blocks.
6. CRC-32 the complete 256 KiB bootloader region.
7. Back up one non-loader 64 KiB sector into RAM.
8. Erase and verify the sector.
9. Program a deterministic pattern through every page.
10. Read back and verify every byte.
11. Erase, restore and verify the original sector.
12. CRC-32 the bootloader again and require an exact match.
13. Emit a terminal pass result and an atomic host JSON receipt.

The target rejects any scratch address below `0x00040000`, any unaligned range,
any range other than exactly one erase block and any request outside the 16 MiB
part. The default scratch sector is `0x00ff0000`.

## Invocation

```sh
./tools/firmware-flasher/firmware-flasher.sh \
  --bootloader-preflight \
  --recovery-path ram-upload \
  --target-model MS42P \
  --serial-device /dev/serial/by-id/<adapter>
```

No firmware image or release manifest is required. A passing receipt records
model, SoC family, recovery payload SHA-256, negotiated baud, selected frame and
the selected window, optional sparse/LZ4 capabilities, scratch address and restoration
result.

## Safety boundary

The test is destructive while it is running. A power loss can leave the chosen
scratch sector erased or partially programmed. It never intentionally targets
the bootloader region, and success requires the complete before/after
bootloader CRC-32 to match.

The machine-readable contract is:

```text
hardware_preflight_contract=spi-nor-scratch-rw-restore-loader-crc-v4
PREFLIGHT=4
```
