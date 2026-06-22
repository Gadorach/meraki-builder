# Jaguar1 recovery JEDEC `ffffff` correction

`GENERAL_CTRL.IF_MASTER_SPI_ENA` was successfully enabled, but the recovery
stage still read `ffffff` because its software-SPI chip-select values were
inverted. The MSCC controller interprets `SW_SPI_CS` as an active mask, not as
four active-low output levels.

The corrected stage asserts the boot NOR using `BIT(0)`, deselects by clearing
the CS field, and follows the working MSCC U-Boot mode-0 transfer sequence.

A new `PREFLIGHT=3` / `spi-nor-scratch-rw-restore-loader-crc-v3` contract forces
all affected recovery payloads to rebuild. Run `REBUILD_LOADER=1 make all` and
then retry `--bootloader-preflight`; no firmware image transfer is required.
