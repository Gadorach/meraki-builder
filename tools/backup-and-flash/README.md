# Backup and full-NOR flash

Uses flashrom with a CH341A-compatible programmer to read the complete 16 MiB
NOR multiple times, require byte-identical backups, write a complete
RedBoot/LinuxLoader postmerkOS image, and compare an explicit full readback.

This is a hardware recovery/install tool, not the in-system firmware updater.
The switch must remain unpowered whenever the SPI programmer is connected.
After a verified write it can locate and launch the separate `serial-console`
tool.
