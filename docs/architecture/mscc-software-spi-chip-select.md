# MSCC software-SPI chip-select contract

The `ICPU_CFG:SPI_MST:SW_MODE.SW_SPI_CS` field is an **active mask**.
For the boot NOR on chip select zero:

- `SW_SPI_CS = BIT(0)` asserts CS0;
- `SW_SPI_CS = 0` deselects every chip select;
- `SW_SPI_CS_OE = BIT(0)` enables the CS0 output driver.

The earlier recovery implementation treated the four CS bits as active-low pin
levels and used `0xe` for CS0-active and `0xf` for deselect. That is the inverse
of the working MSCC U-Boot driver and leaves the boot flash unselected, causing
an all-high `ffffff` JEDEC read.

Recovery preflight contract v3 ports the known-good U-Boot mode-0 sequence:

1. assert CS0 with the active mask and start SCK high;
2. drive data with SCK low;
3. raise SCK and sample SDI near the following falling edge;
4. clear the CS field to deselect;
5. release SCK output-enable and then clear `SW_MODE`.

The target prints the following before its JEDEC probe:

```text
PMOSREC SPI-CS-CONTRACT ACTIVE-MASK CS0=00000001 NONE=00000000
```

Current payloads require `PREFLIGHT=4` and
`spi-nor-scratch-rw-restore-loader-crc-v4`; older chip-select implementations are rejected by the builder and host
flasher so the inverted-CS implementation cannot be reused from cache.
