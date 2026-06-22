# Host smoke test

Runs syntax checks for all non-research tools and target updater scripts,
compiles and tests the manifest helper and static flasher, validates both modern
and checksum-only compatibility publication paths, tests the firmware flasher's private TFTP server,
and exercises a non-destructive fwflash preflight failure.

No serial, SPI, MTD, or switch hardware is accessed.
